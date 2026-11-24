// Resolve reachable declarations and capture the external action inputs for a build request.
export module ycxx.build.plan;

import std;
import ycxx.build.action;
import ycxx.build.manifest;
import ycxx.hash;

export namespace ycxx::build {

struct GeneratorPlan {
  std::string name;
  Action action;
  std::vector<ActionInput> inputs;
  std::string tool_fingerprint;
};

struct SourcePlan {
  std::string path;
  std::vector<std::string> includes;
};

struct ProjectPlan {
  std::vector<GeneratorPlan> generators;
  std::vector<SourcePlan> sources;
};

std::string generated_path(const std::filesystem::path& cwd, std::string_view generator,
                           std::string_view filename);
std::expected<ProjectPlan, std::string> plan_project(const std::filesystem::path& cwd, std::string_view target);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

namespace fs = std::filesystem;

std::string path_text(const fs::path& path) {
  return path.generic_native_encoded_string();
}

struct PlanningError {
  std::string message;
};

[[noreturn]] void fail(SourceLocation at, std::string why) {
  throw PlanningError{std::format("build.ycxx:{}:{}: {}", at.line, at.column, why)};
}

template <class T>
const T& as(const Value& value, std::string_view what) {
  if (const auto* data = std::get_if<T>(&value.data)) return *data;
  fail(value.location, std::format("expected {}", what));
}

const Value* field(const TargetDecl& decl, std::string_view name) {
  for (const auto& [key, val] : decl.fields)
    if (key == name) return &val;
  return nullptr;
}

const Value& require_field(const TargetDecl& decl, std::string_view name) {
  if (const auto* val = field(decl, name)) return *val;
  fail(decl.location, std::format("target '{}' requires '{}'", decl.name, name));
}

void check_fields(const TargetDecl& decl, std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, val] : decl.fields)
    if (!std::ranges::contains(allowed, key))
      fail(val.location, std::format("unknown field '{}' for {}", key, decl.kind));
}

bool basename(std::string_view name) {
  return !name.empty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\0');
}

fs::path relative_path(std::string_view text, SourceLocation at, bool allow_root = false) {
  const fs::path path(text);
  if (text.empty() || text.contains('\0') || !path.is_relative() ||
      std::ranges::any_of(path, [](const auto& part) { return part == ".."; }))
    fail(at, std::format("expected a workspace-relative path, got '{}'", text));
  const auto normal = path.lexically_normal();
  if ((!allow_root && (normal == "." || normal.filename().empty())) || *normal.begin() == ".ycxx")
    fail(at, std::format("invalid workspace path '{}'", text));
  return normal;
}

std::string read_file(const fs::path& path, SourceLocation at) {
  std::ifstream in(path, std::ios::binary);
  if (!in) fail(at, std::format("cannot open '{}'", path_text(path)));
  std::string contents;
  std::array<char, 16384> buffer;
  while (in.read(buffer.data(), buffer.size()) || in.gcount() != 0)
    contents.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
  if (!in.eof() || in.bad()) fail(at, std::format("cannot read '{}'", path_text(path)));
  return contents;
}

bool executable(fs::perms permissions) {
  return (permissions & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
}

fs::file_status regular_file(const fs::path& path, SourceLocation at) {
  std::error_code ec;
  const auto status = fs::status(path, ec);
  if (ec || !fs::is_regular_file(status)) fail(at, std::format("'{}' is not a readable file", path_text(path)));
  return status;
}

std::expected<std::vector<DirectoryEntry>, std::string> list_directory(const fs::path& root, std::string_view relative) {
  const auto path = root / relative;
  std::error_code ec;
  std::vector<DirectoryEntry> entries;
  for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = path_text(it->path().filename());
    if (relative == "." && name == ".ycxx") continue;
    const auto status = it->symlink_status(ec);
    if (ec == std::errc::no_such_file_or_directory) {
      ec.clear();
      continue;
    }
    if (ec) break;
    EntryKind kind = EntryKind::other;
    if (fs::is_directory(status)) kind = EntryKind::directory;
    if (fs::is_regular_file(status)) kind = EntryKind::regular;
    entries.push_back({name, kind});
  }
  if (ec == std::errc::no_such_file_or_directory) return std::vector<DirectoryEntry>{};
  if (ec) return std::unexpected(std::format("cannot list '{}': {}", path_text(path), ec.message()));
  return entries;
}

void include_once(std::vector<std::string>& includes, std::string path) {
  if (!std::ranges::contains(includes, path)) includes.push_back(std::move(path));
}

class Planner {
 public:
  Planner(const fs::path& cwd, const Manifest& manifest) : cwd_(cwd) {
    for (const auto& decl : manifest.targets) declarations_.emplace(decl.name, &decl);
  }

  ProjectPlan run(std::string_view target) {
    const auto& root = find(target, {});
    if (root.kind != "cpp.executable") fail(root.location, "build requires a cpp.executable target");
    cxx_target(root);
    if (plan_.sources.empty()) fail(root.location, "build has no C++ source files");
    return std::move(plan_);
  }

 private:
  const TargetDecl& find(std::string_view name, SourceLocation at) const {
    if (!basename(name)) fail(at, std::format("invalid target name '{}'", name));
    const auto match = declarations_.find(name);
    if (match == declarations_.end()) fail(at, std::format("unknown target '{}'", name));
    return *match->second;
  }

  GeneratorPlan generator(const TargetDecl& decl) {
    check_fields(decl, {"tool", "args", "inputs", "outputs"});
    GeneratorPlan plan;
    plan.name = decl.name;
    const auto& tool = require_field(decl, "tool");
    const auto& spelling = as<std::string>(tool, "an absolute tool path");
    if (spelling.contains('\0') || !fs::path(spelling).is_absolute())
      fail(tool.location, "expected an absolute tool path");
    std::error_code ec;
    const auto tool_path = fs::canonical(spelling, ec);
    if (ec) fail(tool.location, std::format("cannot locate tool '{}': {}", spelling, ec.message()));
    const auto status = regular_file(tool_path, tool.location);
    if (!executable(status.permissions())) fail(tool.location, std::format("tool '{}' is not executable", spelling));
    plan.action.tool = spelling;
    const auto fp = Hasher{}.bytes(read_file(tool_path, tool.location)).finish();
    plan.tool_fingerprint = std::format("{:016x}{:016x}", fp.lo, fp.hi);

    std::set<std::string> input_paths;
    if (const auto* entries = field(decl, "inputs")) {
      for (const auto& [name, value] : as<Record>(*entries, "a record of named inputs")) {
        if (name.empty()) fail(value.location, "input name is empty");
        const auto path = relative_path(as<std::string>(value, "an input path"), value.location);
        if (!input_paths.insert(path_text(path)).second) fail(value.location, "duplicate staged input path");
        const auto input_status = regular_file(cwd_ / path, value.location);
        const auto contents = read_file(cwd_ / path, value.location);
        plan.inputs.push_back({name, path_text(path), {{contents.begin(), contents.end()}, executable(input_status.permissions())}});
      }
    }

    std::set<std::string> output_paths;
    const auto& outputs = require_field(decl, "outputs");
    for (const auto& [name, value] : as<Record>(outputs, "a record of named outputs")) {
      const auto& filename = as<std::string>(value, "an output filename");
      if (name.empty() || !basename(filename)) fail(value.location, "expected a named output with a single filename");
      if (!output_paths.insert(filename).second) fail(value.location, "duplicate output filename");
      plan.action.outputs.push_back({name, filename});
    }
    if (plan.action.outputs.empty()) fail(outputs.location, "generator has no declared outputs");

    if (const auto* args = field(decl, "args")) {
      using enum ActionArg::Kind;
      for (const auto& value : as<List>(*args, "a list of action arguments")) {
        if (const auto* text = std::get_if<std::string>(&value.data)) {
          plan.action.args.push_back({literal, *text});
        } else if (const auto* input_name = std::get_if<InputArg>(&value.data)) {
          if (std::ranges::find(plan.inputs, input_name->name, &ActionInput::name) == plan.inputs.end())
            fail(value.location, std::format("unknown input '{}'", input_name->name));
          plan.action.args.push_back({input, input_name->name});
        } else if (const auto* output_name = std::get_if<OutputArg>(&value.data)) {
          if (std::ranges::find(plan.action.outputs, output_name->name, &ActionOutput::name) == plan.action.outputs.end())
            fail(value.location, std::format("unknown output '{}'", output_name->name));
          plan.action.args.push_back({output, output_name->name});
        } else {
          fail(value.location, "expected a string, input(name) or output(name)");
        }
      }
    }
    return plan;
  }

  std::string artifact(const ArtifactRef& ref, SourceLocation at) {
    const auto& decl = find(ref.target, at);
    if (decl.kind != "generate") fail(at, "artifact references require a generate target");
    auto match = generators_.find(decl.name);
    if (match == generators_.end()) {
      auto gen = generator(decl);
      match = generators_.emplace(decl.name, plan_.generators.size()).first;
      plan_.generators.push_back(std::move(gen));
    }
    const auto& outputs = plan_.generators[match->second].action.outputs;
    const auto output = std::ranges::find(outputs, ref.output, &ActionOutput::name);
    if (output == outputs.end()) fail(at, std::format("generator '{}' has no output '{}'", ref.target, ref.output));
    return generated_path(cwd_, decl.name, output->filename);
  }

  const std::vector<std::string>& cxx_target(const TargetDecl& decl) {
    if (const auto it = finished_.find(decl.name); it != finished_.end()) return it->second;
    if (!active_.insert(decl.name).second) fail(decl.location, std::format("dependency cycle at '{}'", decl.name));
    check_fields(decl, {"sources", "deps", "public_include_dirs", "generated_headers"});

    std::vector<std::string> includes;
    if (const auto* dirs = field(decl, "public_include_dirs")) {
      for (const auto& value : as<List>(*dirs, "a list of include directories"))
        include_once(includes, path_text(cwd_ / relative_path(as<std::string>(value, "an include directory"), value.location, true)));
    }
    if (const auto* headers = field(decl, "generated_headers")) {
      for (const auto& value : as<List>(*headers, "a list of generated headers")) {
        const auto path = artifact(as<ArtifactRef>(value, "an artifact reference"), value.location);
        include_once(includes, path_text(fs::path(path).parent_path()));
      }
    }
    if (const auto* deps = field(decl, "deps")) {
      for (const auto& value : as<List>(*deps, "a list of dependencies")) {
        const auto& dep = find(as<std::string>(value, "a library target name"), value.location);
        if (dep.kind != "cpp.library") fail(value.location, std::format("'{}' is not a cpp.library", dep.name));
        for (const auto& path : cxx_target(dep)) include_once(includes, path);
      }
    }

    std::set<std::string> source_paths;
    const auto add = [&](std::string path, SourceLocation at) {
      if (!source_paths.insert(path).second) fail(at, std::format("duplicate source '{}'", path));
      plan_.sources.push_back({std::move(path), includes});
    };
    const auto add_glob = [&](const Glob& glob, SourceLocation at) {
      const auto matches = expand_glob(glob, [&](std::string_view dir) { return list_directory(cwd_, dir); });
      if (!matches) fail(at, matches.error());
      for (const auto& match : *matches) add(path_text(cwd_ / match), at);
    };
    if (const auto* files = field(decl, "sources")) {
      if (const auto* glob = std::get_if<Glob>(&files->data)) {
        add_glob(*glob, files->location);
      } else {
        for (const auto& value : as<List>(*files, "a list of C++ sources or a glob")) {
          if (const auto* path = std::get_if<std::string>(&value.data)) {
            add(path_text(cwd_ / relative_path(*path, value.location)), value.location);
          } else if (const auto* ref = std::get_if<ArtifactRef>(&value.data)) {
            add(artifact(*ref, value.location), value.location);
          } else if (const auto* glob = std::get_if<Glob>(&value.data)) {
            add_glob(*glob, value.location);
          } else {
            fail(value.location, "expected a source path, glob or artifact reference");
          }
        }
      }
    }
    active_.erase(decl.name);
    return finished_.emplace(decl.name, std::move(includes)).first->second;
  }

  const fs::path& cwd_;
  std::map<std::string, const TargetDecl*, std::less<>> declarations_;
  std::map<std::string, std::size_t> generators_;
  std::set<std::string> active_;
  std::map<std::string, std::vector<std::string>> finished_;
  ProjectPlan plan_;
};

}  // namespace

std::string generated_path(const fs::path& cwd, std::string_view generator, std::string_view filename) {
  return path_text(cwd / ".ycxx" / "virtual" / generator / filename);
}

std::expected<ProjectPlan, std::string> plan_project(const fs::path& cwd, std::string_view target) {
  std::error_code ec;
  const auto root = fs::absolute(cwd, ec).lexically_normal();
  if (ec) return std::unexpected(std::format("cannot resolve workspace directory: {}", ec.message()));
  try {
    const auto path = root / "build.ycxx";
    (void)regular_file(path, {});
    auto manifest = parse_manifest(read_file(path, {}));
    if (!manifest) fail(manifest.error().location, std::move(manifest.error().message));
    return Planner(root, *manifest).run(target);
  } catch (PlanningError& error) {
    return std::unexpected(std::move(error.message));
  }
}

}  // namespace ycxx::build
