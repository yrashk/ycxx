// Resolve reachable declarations and capture the external action inputs for a build request.
export module ycxx.build.plan;

import std;
import ycxx.build.action;
import ycxx.build.inspection_model;
import ycxx.build.manifest;
import ycxx.build.rule_protocol;
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

struct ProjectWatchState {
  Fp128 stamp;
  std::vector<std::string> paths;
};

struct RulePackagePlan {
  Action action;
  std::vector<ActionInput> inputs;
  std::string tool_fingerprint;
};

std::string generated_path(const std::filesystem::path& cwd, std::string_view generator,
                           std::string_view filename);
std::expected<ProjectPlan, std::string> plan_project(const std::filesystem::path& cwd, std::string_view target);
// Reobserve planning inputs without launching any actions. Native source contents and include
// probes are tracked separately by the daemon's file table.
ProjectWatchState watch_project(const std::filesystem::path& cwd, std::string_view target);
std::expected<PlanInspection, std::string> inspect_project(const std::filesystem::path& cwd, std::string_view target);
std::expected<std::variant<ProjectPlan, RuleDemand>, std::string> plan_with_rules(
    const std::filesystem::path& cwd, std::string_view target, std::span<const RuleExpansion> expansions);
std::expected<RulePackagePlan, std::string> plan_rule_package(
    const std::filesystem::path& cwd, std::string_view package, std::string_view sdk_header);

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

struct WatchObservations {
  std::optional<Fp128> manifest;
  std::set<std::string> paths;
  // Keep observations made before a failed plan, when there is no ProjectPlan to fingerprint.
  std::vector<Fp128> values;
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

struct IncludeOrigin {
  std::string text;
  SourceLocation location;
};

struct TargetIncludes {
  std::vector<std::string> paths;
  std::vector<IncludeOrigin> origins;
};

std::string artifact_origin(const ArtifactRef& ref) {
  return std::format("artifact({:?}, {:?})", ref.target, ref.output);
}

std::string glob_origin(const Glob& glob) {
  std::string text = std::format("glob({:?}", glob.pattern);
  if (!glob.exclusions.empty()) {
    text += ", exclude = [";
    for (std::size_t i = 0; i < glob.exclusions.size(); ++i) {
      if (i != 0) text += ", ";
      text += std::format("{:?}", glob.exclusions[i]);
    }
    text += ']';
  }
  text += ')';
  return text;
}

// Normalize arguments with the manifest grammar so a rule can pass structured fields through.
void quote(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  out += '"';
}

void append_value(std::string& out, const Value& value);

void append_record(std::string& out, const Record& fields, bool omit_reads = false) {
  out += '{';
  for (const auto& [key, value] : fields) {
    if (omit_reads && key == "reads") continue;
    out += ' ';
    quote(out, key);
    out += " = ";
    append_value(out, value);
  }
  out += " }";
}

void append_value(std::string& out, const Value& value) {
  std::visit([&]<class T>(const T& data) {
    if constexpr (std::same_as<T, std::string>) {
      quote(out, data);
    } else if constexpr (std::same_as<T, List>) {
      out += '[';
      for (std::size_t i = 0; i < data.size(); ++i) {
        if (i != 0) out += ", ";
        append_value(out, data[i]);
      }
      out += ']';
    } else if constexpr (std::same_as<T, Record>) {
      append_record(out, data);
    } else if constexpr (std::same_as<T, Glob>) {
      out += "glob(";
      quote(out, data.pattern);
      if (!data.exclusions.empty()) {
        out += ", exclude = [";
        for (std::size_t i = 0; i < data.exclusions.size(); ++i) {
          if (i != 0) out += ", ";
          quote(out, data.exclusions[i]);
        }
        out += ']';
      }
      out += ')';
    } else if constexpr (std::same_as<T, ArtifactRef>) {
      out += "artifact(";
      quote(out, data.target);
      out += ", ";
      quote(out, data.output);
      out += ')';
    } else {
      if constexpr (std::same_as<T, InputArg>) out += "input(";
      else out += "output(";
      quote(out, data.name);
      out += ')';
    }
  }, value.data);
}

RuleDemand rule_demand(const TargetDecl& decl, std::string_view rule, const std::string& package) {
  RuleDemand demand{decl.name, std::string(rule), package, {}, {}, {}};
  append_record(demand.definition, decl.fields, true);
  for (const auto& [name, value] : decl.fields) {
    if (name == "reads") {
      std::set<std::string> paths;
      for (const auto& entry : as<List>(value, "a list of rule read paths")) {
        const auto path = relative_path(as<std::string>(entry, "a rule read path"), entry.location);
        const auto spelling = path_text(path);
        if (!paths.insert(spelling).second) fail(entry.location, "duplicate rule read path");
        demand.reads.push_back(spelling);
      }
    } else if (const auto* text = std::get_if<std::string>(&value.data)) {
      if (name.empty()) fail(value.location, "rule attribute name is empty");
      demand.attributes.push_back({name, *text});
    }
  }
  return demand;
}

TargetDecl primitive(const TargetDecl& original, const RuleExpansion& expansion) {
  auto parsed = parse_manifest(expansion.definition);
  if (!parsed)
    fail(original.location, std::format("invalid expansion for '{}': {}", original.name, parsed.error().message));
  if (!parsed->plugins.empty() || parsed->targets.size() != 1 ||
      parsed->targets.front().name != original.name)
    fail(original.location, std::format("rule must emit one declaration named '{}'", original.name));
  auto decl = std::move(parsed->targets.front());
  if (decl.kind == "native.executable") decl.kind = "cpp.executable";
  else if (decl.kind == "native.library") decl.kind = "cpp.library";
  else if (decl.kind == "action.generate") decl.kind = "generate";
  else fail(original.location, std::format("rule emitted unsupported primitive '{}'", decl.kind));
  decl.location = original.location;
  return decl;
}

class Planner {
 public:
  Planner(const fs::path& cwd, const Manifest& manifest, WatchObservations* watch, PlanInspection* inspection,
          std::optional<std::span<const RuleExpansion>> expansions = std::nullopt)
      : cwd_(cwd), watch_(watch), inspection_(inspection), expansions_(expansions) {
    for (const auto& decl : manifest.targets) declarations_.emplace(decl.name, &decl);
    for (const auto& imported : manifest.plugins)
      prefixes_.emplace(imported.alias, path_text(relative_path(imported.package, imported.location)));
  }

  ProjectPlan run(std::string_view target) {
    const auto& root = find(target, {});
    if (inspection_ && root.kind == "generate") {
      resolve_generator(root);
      return std::move(plan_);
    }
    if (root.kind != "cpp.executable" && !(inspection_ && root.kind == "cpp.library"))
      fail(root.location, "build requires a cpp.executable target");
    cxx_target(root);
    if (root.kind == "cpp.executable" && plan_.sources.empty()) fail(root.location, "build has no C++ source files");
    return std::move(plan_);
  }

  RulePackagePlan package(std::string_view target, std::string_view sdk_header) {
    const auto& decl = find(target, {});
    if (decl.kind != "host_action") fail(decl.location, "rule package target must be a host_action");
    auto plan = generator(decl, sdk_header);
    if (plan.action.outputs.size() != 1 || plan.action.outputs.front().name != "plugin")
      fail(decl.location, "rule package must declare exactly one output named 'plugin'");
    return {std::move(plan.action), std::move(plan.inputs), std::move(plan.tool_fingerprint)};
  }

 private:
  std::string display_path(const fs::path& path) const {
    const auto relative = path.lexically_relative(cwd_);
    return relative.empty() || *relative.begin() == ".." ? path_text(path) : path_text(relative);
  }

  void fact(std::string_view kind, const TargetDecl& target, std::string_view name, std::string_view value,
            std::string_view origin, SourceLocation at) {
    inspection_->facts.push_back({std::string(kind), target.name, std::string(name), std::string(value),
                                  std::string(origin), at.line, at.column});
  }

  void observe(const fs::path& path) const {
    if (!watch_) return;
    auto normalized = path.lexically_normal();
    if (normalized != normalized.root_path() && normalized.filename().empty())
      normalized = normalized.parent_path();
    watch_->paths.insert(path_text(normalized));
  }

  const TargetDecl& find(std::string_view name, SourceLocation at) {
    if (!basename(name)) fail(at, std::format("invalid target name '{}'", name));
    const auto match = declarations_.find(name);
    if (match == declarations_.end()) fail(at, std::format("unknown target '{}'", name));
    const auto& decl = *match->second;
    const auto dot = decl.kind.find('.');
    if (dot == std::string::npos) return decl;
    const auto prefix = prefixes_.find(std::string_view(decl.kind).substr(0, dot));
    if (prefix == prefixes_.end()) return decl;
    if (const auto resolved = primitives_.find(name); resolved != primitives_.end()) return resolved->second;
    if (!expansions_)
      fail(decl.location, std::format("plugin rule '{}' requires rule expansion", decl.kind));
    const auto suffix = std::string_view(decl.kind).substr(dot + 1);
    if (suffix.empty()) fail(decl.location, "missing imported rule name");
    auto demand = rule_demand(decl, suffix, prefix->second);
    const auto expansion = std::ranges::find(*expansions_, demand, &RuleExpansion::demand);
    if (expansion == expansions_->end()) throw demand;
    return primitives_.emplace(decl.name, primitive(decl, *expansion)).first->second;
  }

  GeneratorPlan generator(const TargetDecl& decl, std::optional<std::string_view> sdk_header = std::nullopt) {
    check_fields(decl, {"tool", "args", "inputs", "outputs"});
    if (inspection_) fact("rule", decl, decl.name, decl.kind, "declared rule", decl.location);
    GeneratorPlan plan;
    plan.name = decl.name;
    const auto& tool = require_field(decl, "tool");
    const auto& spelling = as<std::string>(tool, "an absolute tool path");
    if (spelling.contains('\0') || !fs::path(spelling).is_absolute())
      fail(tool.location, "expected an absolute tool path");
    plan.action.tool = spelling;
    if (inspection_) {
      fact("tool", decl, "tool", display_path(spelling), "declared in tool", tool.location);
    } else {
      observe(spelling);
      std::error_code ec;
      const auto tool_path = fs::canonical(spelling, ec);
      if (ec) fail(tool.location, std::format("cannot locate tool '{}': {}", spelling, ec.message()));
      observe(tool_path);
      const auto status = regular_file(tool_path, tool.location);
      if (!executable(status.permissions())) fail(tool.location, std::format("tool '{}' is not executable", spelling));
      const auto fp = Hasher{}.bytes(read_file(tool_path, tool.location)).finish();
      if (watch_) watch_->values.push_back(fp);
      plan.tool_fingerprint = std::format("{:016x}{:016x}", fp.lo, fp.hi);
    }

    std::set<std::string> input_paths;
    if (sdk_header) {
      std::string sdk_path = "sdk/ycxx_rule_api.h";
      input_paths.insert(sdk_path);
      plan.inputs.push_back({"ycxx_rule_api.h", std::move(sdk_path),
                             {{sdk_header->begin(), sdk_header->end()}, false}});
    }
    if (const auto* entries = field(decl, "inputs")) {
      for (const auto& [name, value] : as<Record>(*entries, "a record of named inputs")) {
        if (name.empty()) fail(value.location, "input name is empty");
        if (sdk_header && name == "ycxx_rule_api.h") fail(value.location, "rule SDK input name is reserved");
        const auto path = relative_path(as<std::string>(value, "an input path"), value.location);
        const auto staged = path_text(path);
        if (!input_paths.insert(staged).second) fail(value.location, "duplicate staged input path");
        Artifact artifact;
        if (inspection_) {
          fact("input", decl, name, staged, "declared in inputs", value.location);
        } else {
          observe(cwd_ / path);
          const auto input_status = regular_file(cwd_ / path, value.location);
          const auto contents = read_file(cwd_ / path, value.location);
          artifact.executable = executable(input_status.permissions());
          if (watch_) watch_->values.push_back(fingerprint(std::tie(contents, artifact.executable)));
          artifact.bytes.assign(contents.begin(), contents.end());
        }
        plan.inputs.push_back({name, staged, std::move(artifact)});
      }
    }

    std::set<std::string> output_paths;
    const auto& outputs = require_field(decl, "outputs");
    for (const auto& [name, value] : as<Record>(outputs, "a record of named outputs")) {
      const auto& filename = as<std::string>(value, "an output filename");
      if (name.empty() || !basename(filename)) fail(value.location, "expected a named output with a single filename");
      if (!output_paths.insert(filename).second) fail(value.location, "duplicate output filename");
      plan.action.outputs.push_back({name, filename});
      if (inspection_)
        fact("output", decl, name, display_path(generated_path(cwd_, decl.name, filename)),
             "declared in outputs", value.location);
    }
    if (plan.action.outputs.empty()) fail(outputs.location, "generator has no declared outputs");

    if (const auto* args = field(decl, "args")) {
      using enum ActionArg::Kind;
      for (const auto& value : as<List>(*args, "a list of action arguments")) {
        const auto argv_fact = [&](std::string_view argument, std::string_view origin) {
          fact("argv", decl, std::to_string(plan.action.args.size()), argument, origin, value.location);
        };
        if (const auto* text = std::get_if<std::string>(&value.data)) {
          if (inspection_) argv_fact(*text, "declared in args");
          plan.action.args.push_back({literal, *text});
        } else if (const auto* input_name = std::get_if<InputArg>(&value.data)) {
          const auto binding = std::ranges::find(plan.inputs, input_name->name, &ActionInput::name);
          if (binding == plan.inputs.end())
            fail(value.location, std::format("unknown input '{}'", input_name->name));
          if (inspection_) argv_fact("inputs/" + binding->path, std::format("input({:?})", binding->name));
          plan.action.args.push_back({input, input_name->name});
        } else if (const auto* output_name = std::get_if<OutputArg>(&value.data)) {
          const auto binding = std::ranges::find(plan.action.outputs, output_name->name, &ActionOutput::name);
          if (binding == plan.action.outputs.end())
            fail(value.location, std::format("unknown output '{}'", output_name->name));
          if (inspection_) argv_fact("outputs/" + binding->filename, std::format("output({:?})", binding->name));
          plan.action.args.push_back({output, output_name->name});
        } else {
          fail(value.location, "expected a string, input(name) or output(name)");
        }
      }
    }
    return plan;
  }

  const GeneratorPlan& resolve_generator(const TargetDecl& decl) {
    auto match = generators_.find(decl.name);
    if (match == generators_.end()) {
      auto gen = generator(decl);
      match = generators_.emplace(decl.name, plan_.generators.size()).first;
      plan_.generators.push_back(std::move(gen));
    }
    return plan_.generators[match->second];
  }

  std::string artifact(const ArtifactRef& ref, SourceLocation at) {
    const auto& decl = find(ref.target, at);
    if (decl.kind != "generate") fail(at, "artifact references require a generate target");
    const auto& outputs = resolve_generator(decl).action.outputs;
    const auto output = std::ranges::find(outputs, ref.output, &ActionOutput::name);
    if (output == outputs.end()) fail(at, std::format("generator '{}' has no output '{}'", ref.target, ref.output));
    return generated_path(cwd_, decl.name, output->filename);
  }

  const TargetIncludes& cxx_target(const TargetDecl& decl) {
    if (const auto it = finished_.find(decl.name); it != finished_.end()) return it->second;
    if (!active_.insert(decl.name).second) fail(decl.location, std::format("dependency cycle at '{}'", decl.name));
    check_fields(decl, {"sources", "deps", "public_include_dirs", "generated_headers"});
    if (inspection_) fact("rule", decl, decl.name, decl.kind, "declared rule", decl.location);

    TargetIncludes includes;
    const auto include = [&](std::string path, std::string_view origin, SourceLocation location) {
      if (std::ranges::contains(includes.paths, path)) return;
      if (inspection_) {
        fact("include", decl, std::to_string(includes.paths.size()), display_path(path), origin, location);
        includes.origins.push_back({std::string(origin), location});
      }
      includes.paths.push_back(std::move(path));
    };
    if (const auto* dirs = field(decl, "public_include_dirs")) {
      for (const auto& value : as<List>(*dirs, "a list of include directories")) {
        const auto path = cwd_ / relative_path(as<std::string>(value, "an include directory"), value.location, true);
        observe(path);
        include(path_text(path), "declared in public_include_dirs", value.location);
      }
    }
    if (const auto* headers = field(decl, "generated_headers")) {
      for (const auto& value : as<List>(*headers, "a list of generated headers")) {
        const auto& ref = as<ArtifactRef>(value, "an artifact reference");
        const auto path = artifact(ref, value.location);
        include(path_text(fs::path(path).parent_path()), inspection_ ? artifact_origin(ref) : std::string{}, value.location);
      }
    }
    if (const auto* deps = field(decl, "deps")) {
      std::size_t position = 0;
      for (const auto& value : as<List>(*deps, "a list of dependencies")) {
        const auto& dep = find(as<std::string>(value, "a library target name"), value.location);
        if (dep.kind != "cpp.library") fail(value.location, std::format("'{}' is not a cpp.library", dep.name));
        if (inspection_) fact("dependency", decl, std::to_string(position), dep.name, "declared in deps", value.location);
        ++position;
        const auto& inherited = cxx_target(dep);
        for (std::size_t i = 0; i < inherited.paths.size(); ++i) {
          if (inspection_) {
            const auto& from = inherited.origins[i];
            include(inherited.paths[i], std::format("inherited from {} ({})", dep.name, from.text), from.location);
          } else {
            include(inherited.paths[i], {}, {});
          }
        }
      }
    }

    std::set<std::string> source_paths;
    const auto add = [&](std::string path, SourceLocation at, std::string_view origin) {
      if (!source_paths.insert(path).second) fail(at, std::format("duplicate source '{}'", path));
      if (inspection_) {
        const auto display = display_path(path);
        fact("source", decl, display, display, origin, at);
      }
      plan_.sources.push_back({std::move(path), includes.paths});
    };
    const auto add_glob = [&](const Glob& glob, SourceLocation at) {
      const auto matches = expand_glob(glob, [&](std::string_view dir) {
        observe(cwd_ / dir);
        return list_directory(cwd_, dir);
      });
      if (!matches) fail(at, matches.error());
      if (watch_) watch_->values.push_back(fingerprint(*matches));
      const auto origin = inspection_ ? glob_origin(glob) : std::string{};
      for (const auto& match : *matches) {
        const auto path = cwd_ / match;
        observe(path);
        add(path_text(path), at, origin);
      }
    };
    if (const auto* files = field(decl, "sources")) {
      if (const auto* glob = std::get_if<Glob>(&files->data)) {
        add_glob(*glob, files->location);
      } else {
        for (const auto& value : as<List>(*files, "a list of C++ sources or a glob")) {
          if (const auto* path = std::get_if<std::string>(&value.data)) {
            const auto absolute = cwd_ / relative_path(*path, value.location);
            observe(absolute);
            add(path_text(absolute), value.location, "declared in sources");
          } else if (const auto* ref = std::get_if<ArtifactRef>(&value.data)) {
            add(artifact(*ref, value.location), value.location, inspection_ ? artifact_origin(*ref) : std::string{});
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
  WatchObservations* watch_;
  PlanInspection* inspection_;
  std::optional<std::span<const RuleExpansion>> expansions_;
  std::map<std::string, const TargetDecl*, std::less<>> declarations_;
  std::map<std::string, std::string, std::less<>> prefixes_;
  std::map<std::string, TargetDecl, std::less<>> primitives_;
  std::map<std::string, std::size_t> generators_;
  std::set<std::string> active_;
  std::map<std::string, TargetIncludes> finished_;
  ProjectPlan plan_;
};

std::expected<ProjectPlan, std::string> read_project(const fs::path& cwd, std::string_view target,
                                                   WatchObservations* watch, PlanInspection* inspection = nullptr,
                                                   std::optional<std::span<const RuleExpansion>> expansions = std::nullopt) {
  std::error_code ec;
  const auto root = fs::absolute(cwd, ec).lexically_normal();
  if (ec) return std::unexpected(std::format("cannot resolve workspace directory: {}", ec.message()));
  try {
    const auto path = root / "build.ycxx";
    if (watch) watch->paths.insert(path_text(path));
    (void)regular_file(path, {});
    auto text = read_file(path, {});
    if (watch) watch->manifest = fingerprint128(text);
    auto manifest = parse_manifest(text);
    if (!manifest) fail(manifest.error().location, std::move(manifest.error().message));
    if (inspection) inspection->definition = std::move(text);
    return Planner(root, *manifest, watch, inspection, expansions).run(target);
  } catch (PlanningError& error) {
    return std::unexpected(std::move(error.message));
  }
}

}  // namespace

std::string generated_path(const fs::path& cwd, std::string_view generator, std::string_view filename) {
  return path_text(cwd / ".ycxx" / "virtual" / generator / filename);
}

std::expected<ProjectPlan, std::string> plan_project(const fs::path& cwd, std::string_view target) {
  return read_project(cwd, target, nullptr);
}

ProjectWatchState watch_project(const fs::path& cwd, std::string_view target) {
  WatchObservations observed;
  const auto plan = read_project(cwd, target, &observed);
  const auto stamp = plan ? fingerprint(std::tie(observed.manifest, *plan)) :
                           fingerprint(std::tie(observed.manifest, observed.values, plan.error()));
  return {stamp, {observed.paths.begin(), observed.paths.end()}};
}

std::expected<PlanInspection, std::string> inspect_project(const fs::path& cwd, std::string_view target) {
  PlanInspection inspection{.target = std::string(target), .definition = {}, .facts = {}};
  auto plan = read_project(cwd, target, nullptr, &inspection);
  if (!plan) return std::unexpected(std::move(plan.error()));
  return inspection;
}

std::expected<std::variant<ProjectPlan, RuleDemand>, std::string> plan_with_rules(
    const fs::path& cwd, std::string_view target, std::span<const RuleExpansion> expansions) {
  try {
    auto plan = read_project(cwd, target, nullptr, nullptr, expansions);
    if (!plan) return std::unexpected(std::move(plan.error()));
    return std::variant<ProjectPlan, RuleDemand>(std::move(*plan));
  } catch (RuleDemand& demand) {
    return std::variant<ProjectPlan, RuleDemand>(std::move(demand));
  }
}

std::expected<RulePackagePlan, std::string> plan_rule_package(
    const fs::path& cwd, std::string_view package, std::string_view sdk_header) {
  std::error_code ec;
  const auto root = fs::absolute(cwd, ec).lexically_normal();
  if (ec) return std::unexpected(std::format("cannot resolve workspace directory: {}", ec.message()));
  try {
    const auto target = relative_path(package, {});
    const auto directory = root / target.parent_path();
    const auto path = directory / "build.ycxx";
    (void)regular_file(path, {});
    auto manifest = parse_manifest(read_file(path, {}));
    if (!manifest) fail(manifest.error().location, std::move(manifest.error().message));
    return Planner(directory, *manifest, nullptr, nullptr).package(path_text(target.filename()), sdk_header);
  } catch (PlanningError& error) {
    return std::unexpected(std::format("rule package '{}': {}", package, error.message));
  }
}

}  // namespace ycxx::build
