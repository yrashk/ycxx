// Resolve reachable declarations and capture the external action inputs for a build request.
module;
#include <unistd.h>

export module ycxx.build.plan;

import std;
import ycxx.build.action;
import ycxx.build.inspection_model;
import ycxx.build.manifest;
import ycxx.build.publication;
import ycxx.build.rule_compilers;
import ycxx.build.rule_protocol;
import ycxx.hash;
import ycxx.pp;

export namespace ycxx::build {

struct ArtifactInputPlan {
  std::string name;
  std::string path;
  std::size_t producer;
  std::size_t output;
};

struct HostArtifactInputPlan {
  std::string name;
  std::string path;
  std::size_t executable;
};

struct GeneratorPlan {
  std::string name;
  Action action;
  std::vector<ActionInput> inputs;
  std::string tool_fingerprint;
  std::vector<ArtifactInputPlan> artifact_inputs = {};
  bool native_outputs = false;
  std::vector<HostArtifactInputPlan> host_inputs = {};
  bool host_dependent = false;
};

struct SourcePlan {
  std::string path;
  std::vector<std::string> includes;
};

struct ArtifactOutputPlan {
  std::size_t producer;
  std::size_t output;
};

struct HostExecutablePlan {
  std::string name;
  std::vector<SourcePlan> sources;
  std::vector<ArtifactOutputPlan> generated_inputs = {};
  std::size_t first_own_source = 0;
  std::vector<ArtifactOutputPlan> link_inputs = {};
};

using LinkInputPlan = ArtifactOutputPlan;

struct ProjectPlan {
  std::vector<GeneratorPlan> generators;
  std::vector<SourcePlan> sources;
  std::vector<ArtifactOutputPlan> runtime_assets = {};
  std::vector<LinkInputPlan> link_inputs = {};
  std::vector<HostExecutablePlan> host_executables = {};
};

struct RuleInputPlan {
  ProjectPlan producers;
  std::vector<ArtifactInputPlan> inputs;
};

struct RuleWitnessPlan {
  GeneratorPlan witness;
  ProjectPlan producers;
  std::optional<std::string> schema;
};

struct ProjectWatchState {
  Fp128 stamp;
  std::vector<std::string> paths;
  std::optional<RuleDemand> pending = std::nullopt;
};

struct RuleToolchain {
  std::string executable;
  std::uint64_t executable_build;
  std::optional<std::string> path;
};

struct RulePackageAction {
  Action action;
  std::string tool_fingerprint;
  bool native = false;
  bool cxx = false;
};

struct RulePackagePlan {
  std::vector<RulePackageAction> candidates;
  std::vector<ActionInput> inputs;
  std::vector<std::string> observed_paths;
  std::vector<std::string> probe_flags;
  std::vector<std::string> probe_inputs;
};

std::string generated_path(const std::filesystem::path& cwd, std::string_view generator,
                           std::string_view filename, std::string_view native_triple = {});
std::string host_generated_path(const std::filesystem::path& cwd, std::string_view generator,
                                std::string_view filename);
// Reobserve planning inputs without launching any actions. Native source contents and include
// probes are tracked separately by the daemon's file table.
ProjectWatchState watch_project(const std::filesystem::path& cwd, std::string_view target,
                                std::span<const RuleExpansion> expansions, std::string_view native_triple = {});
std::expected<PlanInspection, std::string> inspect_project(
    const std::filesystem::path& cwd, std::string_view target, std::span<const RuleExpansion> expansions,
    std::string_view native_triple, ProjectPlan& resolved);
std::expected<std::variant<ProjectPlan, RuleDemand>, std::string> plan_with_rules(
    const std::filesystem::path& cwd, std::string_view target, std::span<const RuleExpansion> expansions,
    std::optional<Fp128>* definition = nullptr, ProjectWatchState* watch = nullptr,
    std::string_view native_triple = {});
std::expected<RulePackageRef, std::string> capability_package(std::string_view package);
std::expected<RulePackagePlan, std::string> plan_rule_package(
    const std::filesystem::path& cwd, const RulePackageRef& package, std::string_view sdk_header,
    const RuleToolchain& toolchain, std::vector<std::string>* failed_paths = nullptr);
// Reobserve the reachable producers of a rule's generated planning inputs.
// Include paths observed before an incomplete plan.
std::expected<RuleInputPlan, std::string> plan_rule_inputs(
    const std::filesystem::path& cwd, const RuleDemand& demand, std::span<const RuleExpansion> expansions,
    std::string_view native_triple, std::vector<std::string>* paths = nullptr);
// Capture the witness and its artifact producers, or demand the first missing rule expansion.
// Include paths observed before an incomplete plan.
std::expected<std::variant<RuleWitnessPlan, RuleDemand>, std::string> plan_rule_witness(
    const std::filesystem::path& cwd, const RuleExpansion& expansion, std::span<const RuleExpansion> expansions,
    std::string_view native_triple, std::vector<std::string>* paths = nullptr);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

namespace fs = std::filesystem;

constexpr char standard_manifest[] = {
#embed "../../resources/rules/standard/build.ycxx"
    , 0};
constexpr char standard_source[] = {
#embed "../../resources/rules/standard/rules.cc"
    , 0};
const RulePackageRef standard_package{RulePackageRef::Kind::standard, "standard/rules"};
constexpr std::string_view builtin_cpp = "builtin:cpp";

constexpr char capabilities_manifest[] = {
#embed "../../resources/rules/capabilities/build.ycxx"
    , 0};
constexpr char capabilities_source[] = {
#embed "../../resources/rules/capabilities/rules.cc"
    , 0};
const RulePackageRef capabilities_package{RulePackageRef::Kind::capabilities, "builtin:capabilities"};

struct EmbeddedInput {
  std::string_view path;
  std::string_view contents;
};

constexpr EmbeddedInput standard_inputs[] = {
    {"rules.cc", {standard_source, sizeof(standard_source) - 1}},
};
constexpr EmbeddedInput capabilities_inputs[] = {
    {"rules.cc", {capabilities_source, sizeof(capabilities_source) - 1}},
};

std::string path_text(const fs::path& path) {
  return path.generic_string<char>();
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

struct ExecutableObservation {
  std::string canonical;
  Fp128 contents;
};

struct HelperToolIdentity {
  std::string_view name;
  std::string_view spelling;
  std::string_view canonical;
  Fp128 contents;
};

std::string tool_digest(Fp128 fp) { return std::format("{:016x}{:016x}", fp.lo, fp.hi); }

[[noreturn]] void fail(SourceLocation at, std::string why) {
  throw PlanningError{std::format("build.ycxx:{}:{}: {}", at.line, at.column, why)};
}

template <class T>
const T& as(const Value& value, std::string_view what) {
  if (const auto* data = std::get_if<T>(&value.data)) return *data;
  fail(value.location, std::format("expected {}", what));
}

const Value* field(const Record& fields, std::string_view name) {
  for (const auto& [key, val] : fields)
    if (key == name) return &val;
  return nullptr;
}

const Value* field(const TargetDecl& decl, std::string_view name) {
  return field(decl.fields, name);
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

void add_input_path(std::set<std::string>& paths, const std::string& path, SourceLocation at) {
  if (paths.contains(path)) fail(at, "duplicate staged input path");
  for (auto parent = fs::path(path).parent_path(); !parent.empty(); parent = parent.parent_path())
    if (paths.contains(path_text(parent))) fail(at, "overlapping staged input paths");
  const auto directory = path + '/';
  const auto child = paths.lower_bound(directory);
  if (child != paths.end() && child->starts_with(directory)) fail(at, "overlapping staged input paths");
  paths.insert(path);
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

RulePackageRef workspace_package(std::string_view text, SourceLocation at) {
  return {RulePackageRef::Kind::workspace, path_text(relative_path(text, at))};
}

RulePackageRef resolve_capability_package(std::string_view text, SourceLocation at) {
  if (text == builtin_cpp) return standard_package;
  if (text.starts_with("builtin:")) fail(at, std::format("unknown built-in probe package '{}'", text));
  return workspace_package(text, at);
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
  bool generated;
};

struct TargetLinkInput {
  LinkInputPlan artifact;
  std::string origin;
  SourceLocation location;
};

struct CxxTargetPlan {
  const TargetDecl* declaration;
  std::size_t order;
  std::vector<std::string> paths;
  std::vector<IncludeOrigin> origins;
  std::vector<const CxxTargetPlan*> dependencies;
  std::vector<TargetLinkInput> link_inputs;
};

struct CxxTraversal {
  std::vector<SourcePlan>& sources;
  bool generated;
  HostExecutablePlan* host = nullptr;
  std::set<std::string> active = {};
  std::map<std::string, CxxTargetPlan> finished = {};
  std::size_t next_order = 0;
};

struct ArchiveOperand {
  const ArtifactRef& ref;
  SourceLocation location;
  std::optional<std::string_view> binding = std::nullopt;
};

ArchiveOperand archive_operand(const Value& value, bool public_input, bool for_host) {
  if (const auto* choices = public_input ? std::get_if<Record>(&value.data) : nullptr) {
    const auto* host = field(*choices, "host");
    const auto* target = field(*choices, "target");
    if (choices->size() != 2 || !host || !target)
      fail(value.location, "public_link_inputs binding requires exactly 'host' and 'target' fields");
    const auto& host_ref = as<ArtifactRef>(*host, "an artifact reference for 'host' in public_link_inputs");
    const auto& target_ref = as<ArtifactRef>(*target, "an artifact reference for 'target' in public_link_inputs");
    return for_host ? ArchiveOperand{host_ref, host->location, "host"}
                    : ArchiveOperand{target_ref, target->location, "target"};
  }
  return {as<ArtifactRef>(value, "an artifact reference"), value.location};
}

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

void append_record(std::string& out, const Record& fields,
                   std::initializer_list<std::string_view> omitted = {}) {
  out += '{';
  for (const auto& [key, value] : fields) {
    if (std::ranges::contains(omitted, key)) continue;
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
    } else if constexpr (std::same_as<T, CxxTool>) {
      out += "cxx(";
      if (data.compiler) quote(out, *data.compiler);
      out += ')';
    } else if constexpr (std::same_as<T, TargetTriple>) {
      out += "target_triple()";
    } else if constexpr (std::same_as<T, HostTriple>) {
      out += "host_triple()";
    } else if constexpr (std::same_as<T, ProbeRef>) {
      fail(value.location, "probe() requires a named rule capability binding");
    } else if constexpr (std::same_as<T, ValueRef>) {
      fail(value.location, "value() requires a named rule capability or fact binding");
    } else {
      if constexpr (std::same_as<T, InputArg>) out += "input(";
      else out += "output(";
      quote(out, data.name);
      out += ')';
    }
  }, value.data);
}

bool has_capability_bindings(const Value& value) {
  const auto* record = std::get_if<Record>(&value.data);
  return record && std::ranges::any_of(*record, [](const auto& binding) {
    return std::holds_alternative<ProbeRef>(binding.second.data) ||
           std::holds_alternative<ValueRef>(binding.second.data);
  });
}

std::vector<RulePlanInput> rule_plan_inputs(const TargetDecl& decl, std::span<const std::string> reads) {
  std::vector<RulePlanInput> result;
  if (const auto* value = field(decl, "plan_inputs")) {
    for (const auto& [name, entry] : as<Record>(*value, "a record of named planning inputs")) {
      if (!basename(name)) fail(entry.location, "expected a planning input name with a single path component");
      if (std::ranges::contains(reads, name))
        fail(entry.location, std::format("planning input '{}' conflicts with a rule read path", name));
      const auto& ref = as<ArtifactRef>(entry, "artifact(target, output) for a planning input");
      result.push_back({name, ref.target, ref.output});
    }
  }
  std::ranges::sort(result, {}, &RulePlanInput::name);
  return result;
}

RuleDemand rule_demand(const TargetDecl& decl, std::string_view rule, const RulePackageRef& package) {
  RuleDemand demand{decl.name, std::string(rule), package, {}, {}, {}};
  const bool capability_header = package.kind == RulePackageRef::Kind::capabilities && rule == "header";
  if (capability_header) {
    for (const auto name : {"namespace", "filename"})
      if (const auto* value = field(decl, name); value && !std::holds_alternative<std::string>(value->data))
        fail(value->location, std::format("expected a string for capability header field '{}'", name));
    (void)require_field(decl, "capabilities");
  }
  for (const auto& [name, value] : decl.fields) {
    if (name == "plan_inputs") continue;
    if (name == "reads") {
      std::set<std::string> paths;
      for (const auto& entry : as<List>(value, "a list of rule read paths")) {
        const auto path = relative_path(as<std::string>(entry, "a rule read path"), entry.location);
        const auto spelling = path_text(path);
        if (!paths.insert(spelling).second) fail(entry.location, "duplicate rule read path");
        demand.reads.push_back(spelling);
      }
    } else if (name == "capabilities" && (capability_header || has_capability_bindings(value))) {
      const auto& bindings = as<Record>(value, "a record of named capability bindings");
      if (bindings.empty()) fail(value.location, "rule capabilities record is empty");
      for (const auto& [binding, entry] : bindings) {
        if (binding.empty()) fail(entry.location, "rule capability name is empty");
        if (const auto* ref = std::get_if<ValueRef>(&entry.data)) {
          demand.capabilities.push_back({binding, {}, {}, RuleValueReference{ref->target, ref->field}});
          continue;
        }
        const auto& ref = as<ProbeRef>(entry, "probe(package, id) or value(target, export) for a rule capability binding");
        auto provider = resolve_capability_package(ref.package, entry.location);
        auto spelling = ref.package == builtin_cpp ? ref.package : std::move(provider.target);
        demand.capabilities.push_back({binding, std::move(spelling), ref.probe});
      }
    } else if (name == "facts") {
      const auto& bindings = as<Record>(value, "a record of named fact bindings");
      if (bindings.empty()) fail(value.location, "rule facts record is empty");
      for (const auto& [binding, entry] : bindings) {
        if (binding.empty()) fail(entry.location, "rule fact name is empty");
        const auto& ref = as<ValueRef>(entry, "value(target, export) for a rule fact binding");
        demand.facts.push_back({binding, {ref.target, ref.field}});
      }
    } else if (const auto* text = std::get_if<std::string>(&value.data)) {
      if (name.empty()) fail(value.location, "rule attribute name is empty");
      demand.attributes.push_back({name, *text});
    }
  }
  demand.plan_inputs = rule_plan_inputs(decl, demand.reads);
  std::ranges::sort(demand.capabilities, {}, &RuleCapabilityBinding::name);
  std::ranges::sort(demand.facts, {}, &RuleFactReference::name);
  if (demand.capabilities.empty()) append_record(demand.definition, decl.fields, {"reads", "facts", "plan_inputs"});
  else append_record(demand.definition, decl.fields, {"reads", "capabilities", "facts", "plan_inputs"});
  return demand;
}

enum class Primitive { unknown, executable, library, generator, observer, host_action, host_executable };
constexpr std::string_view host_executable_output = "executable";

struct LoweredTarget {
  TargetDecl declaration;
  Primitive kind;
};

struct ResolvedTarget {
  const TargetDecl& declaration;
  Primitive kind;
};

Primitive emitted_kind(std::string_view kind) {
  if (kind == "native.executable") return Primitive::executable;
  if (kind == "native.host_executable") return Primitive::host_executable;
  if (kind == "native.library") return Primitive::library;
  if (kind == "action.generate") return Primitive::generator;
  if (kind == "action.observe") return Primitive::observer;
  return Primitive::unknown;
}

void require_value_export(const TargetDecl& decl, Primitive kind, std::string_view requested, SourceLocation at) {
  if (kind != Primitive::observer)
    fail(at, std::format("target '{}' must emit action.observe for value export '{}'", decl.name, requested));
  const auto& name = require_field(decl, "export");
  const auto& actual = as<std::string>(name, "a value export name");
  if (requested.empty() || actual != requested)
    fail(at, std::format("target '{}' exports '{}', requested '{}'", decl.name, actual, requested));
}

std::optional<std::string_view> observation_schema(const TargetDecl& decl) {
  const auto* value = field(decl, "schema");
  if (!value) return std::nullopt;
  const auto& schema = as<std::string>(*value, "an observation schema name");
  if (schema.empty() || schema.contains('\0')) fail(value->location, "invalid observation schema name");
  return schema;
}

void set_location(Value& value, SourceLocation at) {
  value.location = at;
  if (auto* list = std::get_if<List>(&value.data)) {
    for (auto& entry : *list) set_location(entry, at);
  } else if (auto* record = std::get_if<Record>(&value.data)) {
    for (auto& [key, entry] : *record) set_location(entry, at);
  }
}

std::vector<LoweredTarget> primitives(const TargetDecl& original, const RuleExpansion& expansion) {
  auto parsed = parse_manifest(expansion.definition);
  if (!parsed)
    fail(original.location, std::format("invalid expansion for '{}': {}", original.name, parsed.error().message));
  if (!parsed->plugins.empty()) fail(original.location, "rule expansions cannot import plugins");
  const auto entry = std::ranges::find(parsed->targets, original.name, &TargetDecl::name);
  if (entry == parsed->targets.end())
    fail(original.location, std::format("rule must emit a declaration named '{}'", original.name));
  if (parsed->targets.size() > 1 && emitted_kind(entry->kind) != Primitive::generator)
    fail(original.location, "only an action.generate expansion can have private declarations");
  std::ranges::iter_swap(parsed->targets.begin(), entry);

  std::vector<LoweredTarget> lowered;
  lowered.reserve(parsed->targets.size());
  for (auto& decl : parsed->targets) {
    const auto kind = emitted_kind(decl.kind);
    if (kind == Primitive::unknown)
      fail(original.location, std::format("rule emitted unsupported primitive '{}'", decl.kind));
    decl.location = original.location;
    const bool is_public = lowered.empty();
    if (is_public) {
      decl.kind = original.kind;
    } else {
      if (kind != Primitive::generator || !basename(decl.name))
        fail(original.location, "private rule declarations must be action.generate with local names");
      decl.name = original.name + '/' + decl.name;
    }
    for (auto& [key, value] : decl.fields) {
      if (const auto* authored = is_public ? field(original, key) : nullptr) {
        std::string declared, emitted;
        append_value(declared, *authored);
        append_value(emitted, value);
        if (declared == emitted) {
          value = *authored;
          continue;
        }
      }
      set_location(value, original.location);
    }
    lowered.push_back({std::move(decl), kind});
  }
  return lowered;
}

class Planner {
 public:
  Planner(const fs::path& cwd, const Manifest& manifest, WatchObservations* watch, PlanInspection* inspection,
          std::span<const RuleExpansion> expansions = {}, std::string_view native_triple = {})
      : cwd_(cwd), watch_(watch), inspection_(inspection), expansions_(expansions), native_triple_(native_triple) {
    for (const auto& decl : manifest.targets) declarations_.emplace(decl.name, &decl);
    for (const auto& imported : manifest.plugins)
      prefixes_.emplace(imported.alias, imported.package == capabilities_package.target
          ? capabilities_package : workspace_package(imported.package, imported.location));
    prefixes_.try_emplace("cpp", standard_package);
  }

  ProjectPlan run(std::string_view target) {
    const auto [root, kind] = find(target, {});
    if (inspection_ && kind == Primitive::generator) {
      resolve_generator(root);
      return std::move(plan_);
    }
    if (inspection_ && kind == Primitive::host_executable) {
      resolve_host_executable(root);
      return std::move(plan_);
    }
    if (kind != Primitive::executable && !(inspection_ && kind == Primitive::library))
      fail(root.location, "build requires a cpp.executable target");
    CxxTraversal traversal{plan_.sources, true};
    const auto& native = cxx_target(root, kind, traversal);
    if (kind == Primitive::executable && plan_.sources.empty()) fail(root.location, "build has no C++ source files");
    append_link_inputs(native, traversal);
    return std::move(plan_);
  }

  RulePackagePlan package(std::string_view target, std::string_view sdk_header,
                          const fs::path& invoking_directory, const RuleToolchain& toolchain,
                          std::span<const EmbeddedInput> embedded = {}) {
    const auto declaration = declarations_.find(target);
    if (declaration == declarations_.end()) fail({}, std::format("unknown target '{}'", target));
    const auto& decl = *declaration->second;
    if (decl.kind != "host_action") fail(decl.location, "rule package target must be a host_action");
    auto plan = generator(decl, sdk_header, embedded);
    if (plan.action.outputs.size() != 1 || plan.action.outputs.front().name != "plugin")
      fail(decl.location, "rule package must declare exactly one output named 'plugin'");
    const auto& tool = require_field(decl, "tool");
    const auto* selector = std::get_if<CxxTool>(&tool.data);
    const auto* native_args = field(decl, "native_args");
    const auto* probe_flags = field(decl, "probe_flags");
    const auto* probe_inputs = field(decl, "probe_inputs");
    if (!selector) {
      if (native_args) fail(native_args->location, "native_args requires a cxx() tool");
      if (probe_flags) fail(probe_flags->location, "probe_flags requires a cxx() tool");
      if (probe_inputs) fail(probe_inputs->location, "probe_inputs requires a cxx() tool");
      std::vector<RulePackageAction> candidates;
      candidates.push_back({std::move(plan.action), std::move(plan.tool_fingerprint)});
      return {std::move(candidates), std::move(plan.inputs), {}, {}, {}};
    }

    std::vector<std::string> flags;
    if (probe_flags)
      for (const auto& value : as<List>(*probe_flags, "a list of compiler probe flags")) {
        const auto& flag = as<std::string>(value, "a compiler probe flag");
        if (flag.contains('\0')) fail(value.location, "invalid compiler probe flag");
        flags.push_back(flag);
      }
    std::vector<std::string> resources;
    if (probe_inputs)
      for (const auto& value : as<List>(*probe_inputs, "a list of toolchain input paths")) {
        const auto& text = as<std::string>(value, "a toolchain input path");
        if (text.empty() || text.contains('\0')) fail(value.location, "invalid toolchain input path");
        auto path = fs::path(text);
        if (path.is_relative()) path = cwd_ / relative_path(text, value.location);
        observe(path);
        resources.push_back(path_text(path));
      }

    using enum ActionArg::Kind;
    std::vector<ActionArg> prefix{{literal, "cxx"}, {literal, "--no-daemon"}, {literal, "-nostdlib"}};
    if (native_args) {
      for (const auto& value : as<List>(*native_args, "a list of native compiler arguments"))
        prefix.push_back({literal, as<std::string>(value, "a native compiler argument")});
    }
    const auto compilers = discover_rule_compilers(invoking_directory, toolchain.path,
                                                  toolchain.executable, selector->compiler);
    if (!compilers) fail(tool.location, compilers.error());
    if (watch_) watch_->paths.insert(compilers->observed_paths.begin(), compilers->observed_paths.end());
    if (toolchain.path && std::ranges::none_of(plan.action.environment, [](const auto& variable) {
          return variable.first == "PATH";
        })) plan.action.environment.emplace_back("PATH", compilers->search_path);

    std::vector<RulePackageAction> candidates;
    for (const auto& compiler : compilers->tools) {
      auto action = plan.action;
      action.tool = compiler.executable;
      std::string fingerprint;
      if (compiler.native) {
        action.args.insert_range(action.args.begin(), prefix);
        fingerprint = std::format("ycxx:{:016x}", toolchain.executable_build);
      } else {
        try {
          fingerprint = fingerprint_tool(action.tool, tool.location);
        } catch (const PlanningError&) {
          // An executable without a readable identity is unavailable for a tracked action.
          continue;
        }
      }
      candidates.push_back({std::move(action), std::move(fingerprint), compiler.native, true});
    }
    return {std::move(candidates), std::move(plan.inputs), {}, std::move(flags), std::move(resources)};
  }

  RuleInputPlan planning_inputs(const RuleDemand& demand) {
    const auto original = declarations_.find(demand.target);
    if (original == declarations_.end()) fail({}, std::format("unknown target '{}'", demand.target));
    const auto& decl = *original->second;
    if (rule_plan_inputs(decl, demand.reads) != demand.plan_inputs)
      fail(decl.location, std::format("planning inputs changed for target '{}'", decl.name));
    resolving_rules_.emplace(decl.name, planning_edges_);
    auto inputs = resolve_rule_inputs(decl, demand.plan_inputs);
    return {std::move(plan_), std::move(inputs)};
  }

  RuleWitnessPlan witness(const RuleExpansion& expansion) {
    const auto& demand = expansion.demand;
    if (!demand.value_export) fail({}, std::format("target '{}' has no requested value export", demand.target));
    const auto original = declarations_.find(demand.target);
    if (original == declarations_.end()) fail({}, std::format("unknown target '{}'", demand.target));
    const auto lowered = primitives(*original->second, expansion);
    const auto& [decl, kind] = lowered.front();
    require_value_export(decl, kind, *demand.value_export, decl.location);
    const auto schema = observation_schema(decl);
    // A producer may reach this observer through another rule's value bindings.
    resolving_rules_.emplace(decl.name, planning_edges_);
    auto witness = generator(decl, std::nullopt, {}, true);
    return {std::move(witness), std::move(plan_), schema ? std::optional<std::string>(*schema) : std::nullopt};
  }

 private:
  struct HostGeneratedInput {
    std::string_view owner;
    std::string_view kind;
  };

  struct RestoreHostGeneratedInput {
    const HostGeneratedInput*& current;
    const HostGeneratedInput* previous;
    ~RestoreHostGeneratedInput() { current = previous; }
  };

  std::string display_path(const fs::path& path) const {
    const auto relative = path.lexically_relative(cwd_);
    return relative.empty() || *relative.begin() == ".." ? path_text(path) : path_text(relative);
  }

  void fact(std::string_view kind, const TargetDecl& target, std::string_view name, std::string_view value,
            std::string_view origin, SourceLocation at) {
    inspection_->facts.push_back({std::string(kind), target.name, std::string(name), std::string(value),
                                  std::string(origin), at.line, at.column});
  }

  void dependency(const TargetDecl& consumer, std::string_view producer, std::string_view usage, SourceLocation at,
                  std::optional<std::string_view> output = std::nullopt,
                  std::optional<std::string_view> binding = std::nullopt) {
    const auto own = [](std::string_view name) { return std::string(name); };
    inspection_->dependency_graph->edges.push_back({
        consumer.name, std::string(producer), std::string(usage), output.transform(own), binding.transform(own),
        {.line = at.line, .column = at.column}});
  }

  void artifact_dependency(const TargetDecl& consumer, ArtifactOutputPlan artifact, std::string_view usage,
                           SourceLocation at, std::optional<std::string_view> binding = std::nullopt) {
    if (!inspection_) return;
    const auto& generator = plan_.generators[artifact.producer];
    dependency(consumer, generator.name, usage, at, generator.action.outputs[artifact.output].name, binding);
  }

  [[noreturn]] void host_generated_cycle(SourceLocation at) const {
    fail(at, std::format("host executable generated {} dependency cycle at target '{}'",
                         host_generated_input_->kind, host_generated_input_->owner));
  }

  [[noreturn]] void host_generated_dependency_error(SourceLocation at) const {
    fail(at, std::format("generated {}s for host executables require a producer chain without host executable inputs",
                         host_generated_input_->kind));
  }

  void reject_host_generated_dependency(const TargetDecl& executable, SourceLocation at) const {
    if (!host_generated_input_) return;
    if (executable.name == host_generated_input_->owner) host_generated_cycle(at);
    host_generated_dependency_error(at);
  }

  void observe(const fs::path& path) const {
    if (!watch_) return;
    auto normalized = path.lexically_normal();
    if (normalized != normalized.root_path() && normalized.filename().empty())
      normalized = normalized.parent_path();
    watch_->paths.insert(path_text(normalized));
  }

  const ExecutableObservation& observe_tool(std::string_view spelling, SourceLocation at) {
    if (const auto known = tools_.find(spelling); known != tools_.end())
      return known->second;
    std::error_code ec;
    const auto tool_path = fs::canonical(spelling, ec);
    if (ec) fail(at, std::format("cannot locate tool '{}': {}", spelling, ec.message()));
    observe(tool_path);
    const auto status = regular_file(tool_path, at);
    if (!executable(status.permissions())) fail(at, std::format("tool '{}' is not executable", spelling));
    const auto fp = Hasher{}.bytes(read_file(tool_path, at)).finish();
    if (watch_) watch_->values.push_back(fp);
    return tools_.emplace(std::string(spelling), ExecutableObservation{path_text(tool_path), fp}).first->second;
  }

  std::string fingerprint_tool(std::string_view spelling, SourceLocation at) {
    observe(spelling);
    return tool_digest(observe_tool(spelling, at).contents);
  }

  std::vector<HelperToolIdentity> observe_helper_tools(const TargetDecl& decl, const Value& field) {
    const auto& tools = as<Record>(field, "a record of named helper executables");
    // Collect every spelling before accessing the filesystem. Keep `..` for paths whose
    // symlink components affect resolution.
    for (const auto& [name, value] : tools) {
      const auto& path = as<std::string>(value, "an absolute helper executable path");
      if (name.empty() || name.contains('\0')) fail(value.location, "invalid helper executable name");
      if (path.contains('\0') || !fs::path(path).is_absolute())
        fail(value.location, "expected an absolute helper executable path");
      if (watch_) watch_->paths.insert(path);
    }
    std::vector<HelperToolIdentity> result;
    for (const auto& [name, value] : tools) {
      const auto& path = as<std::string>(value, "an absolute helper executable path");
      try {
        const auto& observation = observe_tool(path, value.location);
        if (::access(path.c_str(), X_OK) != 0) fail(value.location, std::format("tool '{}' is not executable", path));
        if (inspection_)
          fact("tool-input", decl, name, path,
               std::format("declared in tool_inputs · canonical {} · fingerprint {}", observation.canonical,
                           tool_digest(observation.contents)), value.location);
        else
          result.push_back({name, path, observation.canonical, observation.contents});
      } catch (const PlanningError& error) {
        if (!inspection_) throw;
        fact("tool-input", decl, name, path, std::format("unavailable · {}", error.message), value.location);
      }
    }
    std::ranges::sort(result, {}, &HelperToolIdentity::name);
    return result;
  }

  std::vector<ArtifactInputPlan> resolve_rule_inputs(const TargetDecl& decl,
                                                    std::span<const RulePlanInput> inputs) {
    std::vector<ArtifactInputPlan> result;
    if (inputs.empty()) return result;
    const auto& bindings = as<Record>(require_field(decl, "plan_inputs"), "a record of named planning inputs");
    constexpr auto host_error = "host executable artifacts are not available in rule planning inputs";
    result.reserve(inputs.size());
    for (const auto& input : inputs) {
      const auto& binding = *field(bindings, input.name);
      const auto& ref = as<ArtifactRef>(binding, "artifact(target, output) for a planning input");
      ++planning_edges_;
      const auto target = find(ref.target, binding.location);
      if (target.kind == Primitive::host_executable) fail(binding.location, host_error);
      const auto requested = generator_artifact(target, ref.output, binding.location);
      --planning_edges_;
      const auto [producer, output] = requested;
      const auto& gen = plan_.generators[producer];
      artifact_dependency(decl, requested, "plan_inputs", binding.location, input.name);
      auto path = display_path(generated_path(cwd_, gen.name, gen.action.outputs[output].filename, native_triple_));
      result.push_back({input.name, std::move(path), producer, output});
    }
    return result;
  }

  ResolvedTarget find(std::string_view name, SourceLocation at,
                      std::optional<std::string_view> value_export = std::nullopt) {
    if (!basename(name)) fail(at, std::format("invalid target name '{}'", name));
    if (const auto active = resolving_rules_.find(name); active != resolving_rules_.end()) {
      if (active->second != planning_edges_)
        fail(at, std::format("planning input dependency cycle at target '{}'", name));
      fail(at, std::format("value dependency cycle at target '{}'", name));
    }
    const auto match = declarations_.find(name);
    if (match == declarations_.end()) fail(at, std::format("unknown target '{}'", name));
    const auto& decl = *match->second;
    if (const auto resolved = primitives_.find(name); resolved != primitives_.end())
      return {resolved->second.declaration, resolved->second.kind};
    if (emitted_kind(decl.kind) != Primitive::unknown)
      fail(decl.location, std::format("primitive '{}' can only be emitted by a rule", decl.kind));
    if (decl.kind == "host_action") return {decl, Primitive::host_action};

    const auto dot = decl.kind.find('.');
    std::string_view rule = decl.kind;
    const RulePackageRef* package = nullptr;
    if (dot != std::string::npos) {
      const auto alias = rule.substr(0, dot);
      rule = rule.substr(dot + 1);
      if (const auto supplied = prefixes_.find(alias); supplied != prefixes_.end())
        package = &supplied->second;
    } else if (decl.kind == "generate") {
      package = &standard_package;
    }
    if (!package) return {decl, Primitive::unknown};
    if (rule.empty()) fail(decl.location, "missing imported rule name");
    resolving_rules_.emplace(decl.name, planning_edges_);
    auto demand = rule_demand(decl, rule, *package);
    if (value_export) demand.value_export = *value_export;
    if (watch_) {
      if (package->kind == RulePackageRef::Kind::workspace)
        observe(cwd_ / fs::path(package->target).parent_path() / "build.ycxx");
      for (const auto& path : demand.reads) observe(cwd_ / path);
      for (const auto& binding : demand.capabilities)
        if (!binding.value && binding.package != builtin_cpp)
          observe(cwd_ / fs::path(binding.package).parent_path() / "build.ycxx");
    }
    // Rule prerequisites resolve separately from the emitted action's input graph.
    const RestoreHostGeneratedInput prerequisites{host_generated_input_, std::exchange(host_generated_input_, nullptr)};
    const auto find_bindings = [&](std::string_view name) {
      const auto* value = field(decl, name);
      return value ? std::get_if<Record>(&value->data) : nullptr;
    };
    const auto require_binding = [&](const RuleValueReference& ref, std::string_view binding,
                                     SourceLocation location, bool structured) {
      const auto [provider, kind] = find(ref.target, location, ref.field);
      require_value_export(provider, kind, ref.field, location);
      if (observation_schema(provider).has_value() != structured)
        fail(location, std::format("value export '{}.{}' requires a {} binding", ref.target, ref.field,
                                  structured ? "capability" : "fact"));
      if (inspection_)
        inspection_->dependency_graph->value_edges->push_back({
            decl.name, provider.name, structured ? "facts" : "capabilities", std::string(binding), ref.field,
            {.line = location.line, .column = location.column}});
    };
    const auto* capabilities = find_bindings("capabilities");
    const auto* facts = find_bindings("facts");
    for (const auto& binding : demand.capabilities) {
      if (binding.value)
        require_binding(*binding.value, binding.name, field(*capabilities, binding.name)->location, false);
    }
    for (const auto& binding : demand.facts)
      require_binding(binding.value, binding.name, field(*facts, binding.name)->location, true);
    (void)resolve_rule_inputs(decl, demand.plan_inputs);
    const auto expansion = std::ranges::find(expansions_, demand, &RuleExpansion::demand);
    if (expansion == expansions_.end()) {
      if (inspection_)
        fact("unresolved", decl, decl.kind, demand.package.target,
             "no current admitted rule expansion. Build this target to resolve it", decl.location);
      throw demand;
    }
    auto group = primitives(decl, *expansion);
    for (const auto& target : group) {
      if (target.declaration.name != decl.name && declarations_.contains(target.declaration.name))
        fail(decl.location, std::format("private action '{}' conflicts with a workspace declaration",
                                       target.declaration.name));
    }
    for (auto& target : group) {
      auto name = target.declaration.name;
      primitives_.emplace(std::move(name), std::move(target));
    }
    const auto& lowered = primitives_.find(decl.name)->second;
    if (inspection_) {
      if (lowered.kind == Primitive::observer) generator(lowered.declaration, std::nullopt, {}, true);
      const auto* planning_inputs = find_bindings("plan_inputs");
      for (const auto& observation : expansion->capability_facts) {
        const auto* bindings = observation.kind == "plan-input" ? planning_inputs :
                               observation.kind == "fact-input" ? facts : capabilities;
        const auto* binding = bindings && observation.kind != "value-export" ? field(*bindings, observation.name) : nullptr;
        fact(observation.kind, decl, observation.name, observation.value, observation.origin,
             binding ? binding->location : decl.location);
      }
    }
    resolving_rules_.erase(decl.name);
    return {lowered.declaration, lowered.kind};
  }

  void stage_input(ActionInput& input, SourceLocation at, std::span<const EmbeddedInput> embedded = {}) {
    auto& artifact = input.contents;
    if (!embedded.empty()) {
      const auto entry = std::ranges::find(embedded, input.path, &EmbeddedInput::path);
      if (entry == embedded.end())
        fail(at, std::format("rule package has no embedded input '{}'", input.path));
      artifact.bytes.assign(entry->contents.begin(), entry->contents.end());
      return;
    }
    const auto path = cwd_ / input.path;
    observe(path);
    const auto status = regular_file(path, at);
    const auto contents = read_file(path, at);
    artifact.executable = executable(status.permissions());
    if (watch_) watch_->values.push_back(fingerprint(std::tie(contents, artifact.executable)));
    artifact.bytes.assign(contents.begin(), contents.end());
  }

  static std::optional<ActionArg::Kind> triple_selector_kind(const Value& value) {
    if (std::holds_alternative<HostTriple>(value.data)) return ActionArg::Kind::host_triple;
    if (std::holds_alternative<TargetTriple>(value.data)) return ActionArg::Kind::target_triple;
    return std::nullopt;
  }

  std::string_view resolve_action_triple(ActionArg::Kind kind, SourceLocation at) const {
    const bool use_host = kind == ActionArg::Kind::host_triple || native_triple_.empty();
    const auto* host = pp::native_target;
    if (use_host && !host) fail(at, "no default native target for this host");
    return use_host ? host->triple : native_triple_;
  }

  GeneratorPlan generator(const TargetDecl& decl, std::optional<std::string_view> sdk_header = std::nullopt,
                          std::span<const EmbeddedInput> embedded = {}, bool observer = false) {
    if (sdk_header)
      check_fields(decl, {"tool", "args", "inputs", "outputs", "environment", "native_args", "probe_flags", "probe_inputs"});
    else if (observer) check_fields(decl, {"export", "schema", "tool", "args", "inputs", "environment", "tool_inputs"});
    else check_fields(decl, {"tool", "args", "inputs", "outputs", "environment", "tool_inputs"});
    if (inspection_) fact("rule", decl, decl.name, decl.kind, "declared rule", decl.location);
    if (inspection_ && observer)
      if (const auto schema = observation_schema(decl))
        fact("schema", decl, "schema", *schema, "declared in schema", field(decl, "schema")->location);
    GeneratorPlan plan;
    plan.name = decl.name;
    const auto& tool = require_field(decl, "tool");
    const bool artifact_inputs_allowed = !sdk_header;
    const auto* staged_tool = artifact_inputs_allowed ? std::get_if<InputArg>(&tool.data) : nullptr;
    if (const auto* spelling = std::get_if<std::string>(&tool.data)) {
      if (spelling->contains('\0') || !fs::path(*spelling).is_absolute())
        fail(tool.location, "expected an absolute tool path");
      plan.action.tool = *spelling;
      if (inspection_) fact("tool", decl, "tool", display_path(*spelling), "declared in tool", tool.location);
      else observe(*spelling);
    } else if (!staged_tool && (!sdk_header || !std::holds_alternative<CxxTool>(tool.data))) {
      if (sdk_header) fail(tool.location, "expected an absolute tool path or cxx() selector");
      fail(tool.location, "expected an absolute tool path or input(name) selector");
    }

    if (const auto* entries = field(decl, "environment")) {
      for (const auto& [name, value] : as<Record>(*entries, "a record of environment variables")) {
        const auto kind = sdk_header ? std::nullopt : triple_selector_kind(value);
        const std::string_view text = kind ? resolve_action_triple(*kind, value.location)
                                           : as<std::string>(value, "an environment value");
        if (name.empty() || name.contains('=') || name.contains('\0') || text.contains('\0'))
          fail(value.location, "invalid action environment entry");
        plan.action.environment.emplace_back(name, text);
        if (inspection_)
          fact("environment", decl, name, text, kind ? triple_arg_selector(*kind) : "declared in environment",
               value.location);
      }
    }

    std::set<std::string> input_paths;
    if (sdk_header) {
      std::string sdk_path = "sdk/ycxx_rule_api.h";
      input_paths.insert(sdk_path);
      plan.inputs.push_back({"ycxx_rule_api.h", std::move(sdk_path),
                             {{sdk_header->begin(), sdk_header->end()}, false}});
    }
    const auto first_file_input = plan.inputs.size();
    const auto* inputs = field(decl, "inputs");
    const auto* bindings = inputs ? &as<Record>(*inputs, "a record of named inputs") : nullptr;
    if (bindings) {
      for (const auto& [name, value] : *bindings) {
        if (name.empty()) fail(value.location, "input name is empty");
        if (sdk_header && name == "ycxx_rule_api.h") fail(value.location, "rule SDK input name is reserved");
        if (artifact_inputs_allowed && std::holds_alternative<ArtifactRef>(value.data)) continue;
        const auto path = relative_path(as<std::string>(value, "an input path"), value.location);
        ActionInput input{name, path_text(path), {}};
        add_input_path(input_paths, input.path, value.location);
        if (inspection_) {
          fact("input", decl, name, input.path, "declared in inputs", value.location);
        } else if (embedded.empty()) {
          observe(cwd_ / path);
        }
        plan.inputs.push_back(std::move(input));
      }
    }
    std::vector<HelperToolIdentity> helpers;
    if (const auto* tool_inputs = field(decl, "tool_inputs")) helpers = observe_helper_tools(decl, *tool_inputs);
    if (!inspection_) {
      // Capture every declared path first so a missing tool or input leaves a watchable recipe.
      if (!plan.action.tool.empty()) plan.tool_fingerprint = fingerprint_tool(plan.action.tool, tool.location);
      if (!helpers.empty()) {
        // Actions without helpers keep the original tool fingerprint and receipt key.
        constexpr std::string_view kind = "ycxx.tool_inputs.v1";
        plan.tool_fingerprint = tool_digest(fingerprint(std::tie(kind, plan.tool_fingerprint, helpers)));
      }
      if (bindings) {
        auto index = first_file_input;
        for (const auto& [name, value] : *bindings)
          if (std::holds_alternative<std::string>(value.data))
            stage_input(plan.inputs[index++], value.location, embedded);
      }
    }
    if (artifact_inputs_allowed && bindings) {
      for (const auto& [name, value] : *bindings) {
        const auto* ref = std::get_if<ArtifactRef>(&value.data);
        if (!ref) continue;
        if (!basename(name)) fail(value.location, "expected an artifact input name with a single path component");
        const auto path_for = [&](std::string_view filename) {
          auto path = "artifacts/" + name + '/' + std::string(filename);
          add_input_path(input_paths, path, value.location);
          if (inspection_) fact("input", decl, name, path, artifact_origin(*ref), value.location);
          return path;
        };
        const auto target = artifact_target(ref->target, value.location, decl.name);
        if (target.kind == Primitive::host_executable) {
          reject_host_generated_dependency(target.declaration, value.location);
          if (observer && (!staged_tool || staged_tool->name != name))
            fail(value.location, std::format("host executable witness input {:?} must be selected with tool = input({:?})",
                                             name, name));
          if (ref->output != host_executable_output)
            fail(value.location, std::format("host executable '{}' has no output '{}'", target.declaration.name,
                                             ref->output));
          const auto executable = resolve_host_executable(target.declaration);
          if (inspection_)
            dependency(decl, target.declaration.name, "inputs", value.location, host_executable_output, name);
          plan.host_inputs.push_back({name, path_for(host_executable_output), executable});
          continue;
        }
        const auto requested = generator_artifact(target, ref->output, value.location);
        artifact_dependency(decl, requested, "inputs", value.location, name);
        const auto [producer, output] = requested;
        const auto& filename = plan_.generators[producer].action.outputs[output].filename;
        plan.artifact_inputs.push_back({name, path_for(filename), producer, output});
      }
    }
    const auto artifact_input_path = [&](std::string_view name) -> std::optional<std::string_view> {
      if (const auto binding = std::ranges::find(plan.artifact_inputs, name, &ArtifactInputPlan::name);
          binding != plan.artifact_inputs.end()) return binding->path;
      if (const auto binding = std::ranges::find(plan.host_inputs, name, &HostArtifactInputPlan::name);
          binding != plan.host_inputs.end()) return binding->path;
      return std::nullopt;
    };
    if (staged_tool) {
      const auto path = artifact_input_path(staged_tool->name);
      if (!path)
        fail(tool.location, std::format("tool input '{}' must name an artifact-backed input", staged_tool->name));
      plan.action.tool = "inputs/" + std::string(*path);
      if (inspection_)
        fact("tool", decl, "tool", plan.action.tool, std::format("input({:?})", staged_tool->name), tool.location);
    }

    if (!observer) {
      std::set<std::string> output_paths;
      const auto& outputs = require_field(decl, "outputs");
      for (const auto& [name, value] : as<Record>(outputs, "a record of named outputs")) {
        const auto& filename = as<std::string>(value, "an output filename");
        if (name.empty() || !basename(filename)) fail(value.location, "expected a named output with a single filename");
        if (!output_paths.insert(filename).second) fail(value.location, "duplicate output filename");
        plan.action.outputs.push_back({name, filename});
        if (inspection_)
          fact("output", decl, name, display_path(generated_path(cwd_, decl.name, filename, native_triple_)),
               "declared in outputs", value.location);
      }
      if (plan.action.outputs.empty()) fail(outputs.location, "generator has no declared outputs");
    }

    if (const auto* args = field(decl, "args")) {
      using enum ActionArg::Kind;
      for (const auto& value : as<List>(*args, "a list of action arguments")) {
        const auto argv_fact = [&](std::string_view argument, std::string_view origin) {
          fact("argv", decl, std::to_string(plan.action.args.size()), argument, origin, value.location);
        };
        if (const auto* text = std::get_if<std::string>(&value.data)) {
          if (inspection_) argv_fact(*text, "declared in args");
          plan.action.args.push_back({literal, *text});
        } else if (const auto kind = triple_selector_kind(value)) {
          const auto selector = triple_arg_selector(*kind);
          if (sdk_header) fail(value.location, std::format("{} is only available in project action arguments", selector));
          const auto triple = resolve_action_triple(*kind, value.location);
          if (inspection_) argv_fact(triple, selector);
          plan.action.args.push_back({*kind, std::string(triple)});
        } else if (const auto* input_name = std::get_if<InputArg>(&value.data)) {
          const auto binding = std::ranges::find(plan.inputs, input_name->name, &ActionInput::name);
          if (binding != plan.inputs.end()) {
            if (inspection_) argv_fact("inputs/" + binding->path, std::format("input({:?})", binding->name));
          } else {
            const auto path = artifact_input_path(input_name->name);
            if (!path)
              fail(value.location, std::format("unknown input '{}'", input_name->name));
            if (inspection_)
              argv_fact("inputs/" + std::string(*path), std::format("input({:?})", input_name->name));
          }
          plan.action.args.push_back({input, input_name->name});
        } else if (const auto* output_name = std::get_if<OutputArg>(&value.data)) {
          const auto binding = std::ranges::find(plan.action.outputs, output_name->name, &ActionOutput::name);
          if (binding == plan.action.outputs.end())
            fail(value.location, std::format("unknown output '{}'", output_name->name));
          if (inspection_) argv_fact("outputs/" + binding->filename, std::format("output({:?})", binding->name));
          plan.action.args.push_back({output, output_name->name});
        } else {
          fail(value.location, "expected a string, input(name), output(name), target_triple() or host_triple()");
        }
      }
    }
    if (!observer && artifact_inputs_allowed) std::ranges::sort(plan.inputs, {}, &ActionInput::name);
    plan.host_dependent = !plan.host_inputs.empty() || std::ranges::any_of(plan.artifact_inputs, [&](const auto& input) {
      return plan_.generators[input.producer].host_dependent;
    });
    return plan;
  }

  std::size_t resolve_generator(const TargetDecl& decl) {
    if (const auto match = generators_.find(decl.name); match != generators_.end()) return match->second;
    const auto [active, inserted] = active_generators_.emplace(decl.name, planning_edges_);
    if (!inserted) {
      if (active->second != planning_edges_)
        fail(decl.location, std::format("planning input dependency cycle at target '{}'", decl.name));
      if (host_generated_input_) host_generated_cycle(decl.location);
      fail(decl.location, std::format("artifact dependency cycle at '{}'", decl.name));
    }
    auto gen = generator(decl);
    active_generators_.erase(decl.name);
    const auto index = plan_.generators.size();
    plan_.generators.push_back(std::move(gen));
    generators_.emplace(decl.name, index);
    return index;
  }

  ResolvedTarget artifact_target(std::string_view name, SourceLocation at, std::string_view consumer) {
    if (!consumer.empty() && basename(name)) {
      const auto scope = consumer.substr(0, consumer.find('/'));
      const auto local = primitives_.find(std::format("{}/{}", scope, name));
      if (local != primitives_.end()) return {local->second.declaration, local->second.kind};
    }
    return find(name, at);
  }

  ArtifactOutputPlan generator_artifact(ResolvedTarget target, std::string_view name, SourceLocation at) {
    const auto [decl, kind] = target;
    if (kind == Primitive::host_executable) {
      reject_host_generated_dependency(decl, at);
      fail(at, "host executable artifacts require a generator input or a directly selected witness tool");
    }
    if (kind != Primitive::generator) fail(at, "artifact references require a generate target");
    const auto producer = resolve_generator(decl);
    const auto& generator = plan_.generators[producer];
    if (host_generated_input_ && generator.host_dependent) host_generated_dependency_error(at);
    const auto& outputs = generator.action.outputs;
    const auto output = std::ranges::find(outputs, name, &ActionOutput::name);
    if (output == outputs.end()) fail(at, std::format("generator '{}' has no output '{}'", decl.name, name));
    return {producer, static_cast<std::size_t>(output - outputs.begin())};
  }

  ArtifactOutputPlan artifact(const ArtifactRef& ref, SourceLocation at) {
    return generator_artifact(find(ref.target, at), ref.output, at);
  }

  std::string native_artifact(const ArtifactRef& ref, const TargetDecl& consumer, std::string_view usage,
                              SourceLocation at, bool emit_edge) {
    const auto requested = artifact(ref, at);
    if (emit_edge) artifact_dependency(consumer, requested, usage, at);
    const auto [producer, output] = requested;
    auto& gen = plan_.generators[producer];
    gen.native_outputs = true;
    return generated_path(cwd_, gen.name, gen.action.outputs[output].filename, native_triple_);
  }

  ArtifactOutputPlan resolve_host_artifact(const ArtifactRef& ref, const HostExecutablePlan& host,
                                           std::string_view kind, SourceLocation at) {
    const HostGeneratedInput input{host.name, kind};
    const RestoreHostGeneratedInput restore{host_generated_input_, std::exchange(host_generated_input_, &input)};
    return artifact(ref, at);
  }

  std::string host_artifact(const ArtifactRef& ref, const TargetDecl& consumer, HostExecutablePlan& host,
                            std::string_view usage, SourceLocation at, bool emit_edge) {
    const auto requested = resolve_host_artifact(ref, host, usage == "sources" ? "source" : "header", at);
    if (emit_edge) artifact_dependency(consumer, requested, usage, at);
    host.generated_inputs.push_back(requested);
    const auto& generator = plan_.generators[requested.producer];
    return host_generated_path(cwd_, generator.name, generator.action.outputs[requested.output].filename);
  }

  void for_each_include_dir(const TargetDecl& decl, const auto& include) {
    if (const auto* dirs = field(decl, "public_include_dirs")) {
      for (const auto& value : as<List>(*dirs, "a list of include directories")) {
        const auto path = cwd_ / relative_path(as<std::string>(value, "an include directory"), value.location, true);
        observe(path);
        include(path_text(path), "declared in public_include_dirs", value.location);
      }
    }
  }

  void append_sources(const TargetDecl& decl, const std::vector<std::string>& includes,
                      std::vector<SourcePlan>& sources, HostExecutablePlan* host, bool emit_facts,
                      bool emit_generated_facts) {
    std::set<std::string> source_paths;
    const auto add = [&](std::string path, SourceLocation at, std::string_view origin, bool generated = false) {
      if (!source_paths.insert(path).second) fail(at, std::format("duplicate source '{}'", path));
      if (generated ? emit_generated_facts : emit_facts) {
        const auto display = display_path(path);
        fact("source", decl, display, display, origin, at);
      }
      sources.push_back({std::move(path), includes});
    };
    const auto add_glob = [&](const Glob& glob, SourceLocation at) {
      const auto matches = expand_glob(glob, [&](std::string_view dir) {
        observe(cwd_ / dir);
        return list_directory(cwd_, dir);
      });
      if (!matches) fail(at, matches.error());
      if (watch_) watch_->values.push_back(fingerprint(*matches));
      const auto origin = emit_facts ? glob_origin(glob) : std::string{};
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
          } else if (const auto* glob = std::get_if<Glob>(&value.data)) {
            add_glob(*glob, value.location);
          } else if (const auto* ref = std::get_if<ArtifactRef>(&value.data)) {
            auto path = host ? host_artifact(*ref, decl, *host, "sources", value.location, emit_facts)
                             : native_artifact(*ref, decl, "sources", value.location, emit_facts);
            add(std::move(path), value.location, emit_generated_facts ? artifact_origin(*ref) : std::string{}, true);
          } else {
            fail(value.location, "expected a source path, glob or artifact reference");
          }
        }
      }
    }
  }

  std::size_t resolve_host_executable(const TargetDecl& decl) {
    const auto existing = std::ranges::find(plan_.host_executables, decl.name, &HostExecutablePlan::name);
    if (existing != plan_.host_executables.end())
      return static_cast<std::size_t>(existing - plan_.host_executables.begin());
    check_fields(decl, {"sources", "public_include_dirs", "deps", "generated_headers", "link_inputs"});
    const auto* host = pp::native_target;
    if (!host) fail(decl.location, "no default native target for this host");
    if (inspection_) {
      fact("rule", decl, decl.name, decl.kind, "declared rule", decl.location);
      fact("host-target", decl, "triple", host->triple, "build host", decl.location);
      fact("output", decl, host_executable_output, host_executable_output, "linked host executable", decl.location);
    }
    HostExecutablePlan tool{decl.name, {}};
    CxxTraversal traversal{tool.sources, false, &tool};
    const auto& native = cxx_target(decl, Primitive::host_executable, traversal);
    if (tool.sources.empty()) fail(decl.location, "host executable has no C++ source files");
    append_link_inputs(native, traversal);
    const auto index = plan_.host_executables.size();
    plan_.host_executables.push_back(std::move(tool));
    return index;
  }

  void append_link_inputs(const CxxTargetPlan& root, const CxxTraversal& traversal) {
    if (std::ranges::none_of(traversal.finished, [](const auto& entry) { return !entry.second.link_inputs.empty(); }))
      return;
    std::vector<std::size_t> consumers(traversal.finished.size());
    for (const auto& [name, target] : traversal.finished)
      for (const auto* dep : target.dependencies) ++consumers[dep->order];
    auto& inputs = traversal.host ? traversal.host->link_inputs : plan_.link_inputs;

    // A shared dependency must follow all its consumers. Among ready targets, keep the
    // order in which the dependency traversal first reached them.
    std::map<std::size_t, const CxxTargetPlan*> ready{{root.order, &root}};
    while (!ready.empty()) {
      const auto* target = ready.begin()->second;
      ready.erase(ready.begin());
      for (const auto& input : target->link_inputs) {
        if (inspection_) {
          const auto [producer, output] = input.artifact;
          const auto& generator = plan_.generators[producer];
          const auto& filename = generator.action.outputs[output].filename;
          const auto path = traversal.host ? host_generated_path(cwd_, generator.name, filename)
                                          : generated_path(cwd_, generator.name, filename, native_triple_);
          auto origin = target == &root ? input.origin :
              std::format("inherited from {} ({})", target->declaration->name, input.origin);
          fact("link-input", *root.declaration, std::to_string(inputs.size()), display_path(path),
               origin, input.location);
        }
        inputs.push_back(input.artifact);
      }
      for (const auto* dep : target->dependencies)
        if (--consumers[dep->order] == 0) ready.emplace(dep->order, dep);
    }
  }

  const CxxTargetPlan& cxx_target(const TargetDecl& decl, Primitive kind, CxxTraversal& traversal) {
    if (const auto it = traversal.finished.find(decl.name); it != traversal.finished.end()) return it->second;
    if (!traversal.active.insert(decl.name).second)
      fail(decl.location, std::format("dependency cycle at '{}'", decl.name));
    if (kind != Primitive::host_executable)
      check_fields(decl, {"sources", "deps", "public_include_dirs", "generated_headers", "runtime_assets",
                          "link_inputs", "public_link_inputs"});
    const auto* assets = field(decl, "runtime_assets");
    if (assets && kind != Primitive::executable) fail(assets->location, "runtime_assets requires an executable target");
    const auto* private_inputs = field(decl, "link_inputs");
    if (private_inputs && kind != Primitive::executable && kind != Primitive::host_executable)
      fail(private_inputs->location, "link_inputs requires an executable target");
    const auto* public_inputs = field(decl, "public_link_inputs");
    if (public_inputs && kind != Primitive::library)
      fail(public_inputs->location, "public_link_inputs requires a library target");
    // Authored edges and disk facts describe declarations. Generated paths
    // belong to the host or selected-target traversal.
    const bool emit_facts = inspection_ && inspected_cxx_.insert(decl.name).second;
    const bool emit_generated_facts = inspection_ &&
        inspected_cxx_variants_.emplace(decl.name, traversal.generated).second;
    if (emit_facts && kind != Primitive::host_executable)
      fact("rule", decl, decl.name, decl.kind, "declared rule", decl.location);

    CxxTargetPlan target{&decl, traversal.next_order++, {}, {}, {}, {}};
    if (const auto* inputs = public_inputs ? public_inputs : private_inputs) {
      const auto field_name = public_inputs ? "public_link_inputs" : "link_inputs";
      for (const auto& value : as<List>(*inputs, "a list of linker archive inputs")) {
        const auto [ref, location, binding] = archive_operand(value, public_inputs != nullptr, traversal.host != nullptr);
        const auto requested = traversal.host ? resolve_host_artifact(ref, *traversal.host, "archive", location)
                                              : artifact(ref, location);
        if (binding ? emit_generated_facts : emit_facts)
          artifact_dependency(decl, requested, field_name, location, binding);
        std::string origin;
        if (inspection_)
          origin = binding ? std::format("declared in {} as {} = {}", field_name, *binding, artifact_origin(ref))
                           : std::format("declared in {} as {}", field_name, artifact_origin(ref));
        target.link_inputs.push_back({requested, std::move(origin), location});
      }
    }
    const auto include = [&](std::string path, std::string_view origin, SourceLocation location, bool generated = false) {
      if (std::ranges::contains(target.paths, path)) return;
      if (inspection_) {
        if (generated ? emit_generated_facts : emit_facts) {
          auto name = std::to_string(target.paths.size());
          if (generated && traversal.host && kind == Primitive::library) name.insert(0, "host ");
          fact("include", decl, name, display_path(path), origin, location);
        }
        target.origins.push_back({std::string(origin), location, generated});
      }
      target.paths.push_back(std::move(path));
    };
    for_each_include_dir(decl, include);
    if (const auto* headers = field(decl, "generated_headers")) {
      for (const auto& value : as<List>(*headers, "a list of generated headers")) {
        const auto& ref = as<ArtifactRef>(value, "an artifact reference");
        const auto path = traversal.host
            ? host_artifact(ref, decl, *traversal.host, "generated_headers", value.location, emit_facts)
            : native_artifact(ref, decl, "generated_headers", value.location, emit_facts);
        include(path_text(fs::path(path).parent_path()), inspection_ ? artifact_origin(ref) : std::string{},
                value.location, true);
      }
    }
    if (const auto* deps = field(decl, "deps")) {
      std::size_t position = 0;
      for (const auto& value : as<List>(*deps, "a list of dependencies")) {
        const auto [dep, dep_kind] = find(as<std::string>(value, "a library target name"), value.location);
        if (dep_kind != Primitive::library) fail(value.location, std::format("'{}' is not a cpp.library", dep.name));
        if (emit_facts) {
          fact("dependency", decl, std::to_string(position), dep.name, "declared in deps", value.location);
          dependency(decl, dep.name, "deps", value.location);
        }
        ++position;
        const auto& inherited = cxx_target(dep, dep_kind, traversal);
        target.dependencies.push_back(&inherited);
        for (std::size_t i = 0; i < inherited.paths.size(); ++i) {
          if (inspection_) {
            const auto& from = inherited.origins[i];
            include(inherited.paths[i], std::format("inherited from {} ({})", dep.name, from.text),
                    from.location, from.generated);
          } else {
            include(inherited.paths[i], {}, {});
          }
        }
      }
    }

    if (kind == Primitive::host_executable) traversal.host->first_own_source = traversal.sources.size();
    append_sources(decl, target.paths, traversal.sources, traversal.host, emit_facts, emit_generated_facts);
    if (assets) {
      std::set<std::string> filenames;
      for (const auto& value : as<List>(*assets, "a list of runtime assets")) {
        const auto& ref = as<ArtifactRef>(value, "an artifact reference");
        const auto asset = artifact(ref, value.location);
        artifact_dependency(decl, asset, "runtime_assets", value.location);
        const auto& filename = plan_.generators[asset.producer].action.outputs[asset.output].filename;
        if (!filenames.insert(filename).second)
          fail(value.location, std::format("duplicate runtime asset filename '{}'", filename));
        if (inspection_)
          fact("runtime-asset", decl, filename,
               path_text(project_state_path({}, native_triple_) / "assets" / decl.name / filename),
               artifact_origin(ref), value.location);
        plan_.runtime_assets.push_back(asset);
      }
    }
    traversal.active.erase(decl.name);
    return traversal.finished.emplace(decl.name, std::move(target)).first->second;
  }

  const fs::path& cwd_;
  WatchObservations* watch_;
  PlanInspection* inspection_;
  std::span<const RuleExpansion> expansions_;
  std::string_view native_triple_;
  std::map<std::string, const TargetDecl*, std::less<>> declarations_;
  std::map<std::string, RulePackageRef, std::less<>> prefixes_;
  std::map<std::string, LoweredTarget, std::less<>> primitives_;
  std::map<std::string, std::size_t> generators_;
  std::map<std::string, ExecutableObservation, std::less<>> tools_;
  // The edge count at each active frame distinguishes mixed cycles from value or artifact cycles.
  std::map<std::string, std::size_t, std::less<>> resolving_rules_;
  std::map<std::string, std::size_t, std::less<>> active_generators_;
  const HostGeneratedInput* host_generated_input_ = nullptr;
  std::size_t planning_edges_ = 0;
  std::set<std::string> inspected_cxx_;
  std::set<std::pair<std::string, bool>> inspected_cxx_variants_;
  ProjectPlan plan_;
};

Manifest read_workspace_manifest(const fs::path& root, WatchObservations* watch, PlanInspection* inspection = nullptr,
                                 std::optional<Fp128>* definition = nullptr) {
  const auto path = root / "build.ycxx";
  if (watch) watch->paths.insert(path_text(path));
  (void)regular_file(path, {});
  auto text = read_file(path, {});
  if (watch || definition) {
    const auto stamp = fingerprint128(text);
    if (watch) watch->manifest = stamp;
    if (definition) *definition = stamp;
  }
  auto manifest = parse_manifest(text);
  if (!manifest) fail(manifest.error().location, std::move(manifest.error().message));
  if (inspection) inspection->definition = std::move(text);
  return std::move(*manifest);
}

std::expected<ProjectPlan, std::string> read_project(const fs::path& cwd, std::string_view target,
                                                   WatchObservations* watch, PlanInspection* inspection,
                                                   std::span<const RuleExpansion> expansions,
                                                   std::optional<Fp128>* definition, std::string_view native_triple) {
  if (definition) definition->reset();
  std::error_code ec;
  const auto root = fs::absolute(cwd, ec).lexically_normal();
  if (ec) return std::unexpected(std::format("cannot resolve workspace directory: {}", ec.message()));
  try {
    const auto manifest = read_workspace_manifest(root, watch, inspection, definition);
    return Planner(root, manifest, watch, inspection, expansions, native_triple).run(target);
  } catch (PlanningError& error) {
    return std::unexpected(std::move(error.message));
  }
}

}  // namespace

std::string generated_path(const fs::path& cwd, std::string_view generator, std::string_view filename,
                           std::string_view native_triple) {
  return path_text(project_state_path(cwd, native_triple) / "virtual" / generator / filename);
}

std::string host_generated_path(const fs::path& cwd, std::string_view generator, std::string_view filename) {
  return path_text(cwd / ".ycxx" / "host" / "virtual" / generator / filename);
}

ProjectWatchState watch_project(const fs::path& cwd, std::string_view target,
                                std::span<const RuleExpansion> expansions, std::string_view native_triple) {
  ProjectWatchState watch;
  (void)plan_with_rules(cwd, target, expansions, nullptr, &watch, native_triple);
  return watch;
}

std::expected<PlanInspection, std::string> inspect_project(
    const fs::path& cwd, std::string_view target, std::span<const RuleExpansion> expansions,
    std::string_view native_triple, ProjectPlan& resolved) {
  PlanInspection inspection{.target = std::string(target), .definition = {}, .facts = {},
                            .dependency_graph = DependencyGraphInspection{
                                .value_edges = std::vector<PlanValueDependencyEdge>{}}};
  auto& graph = *inspection.dependency_graph;
  try {
    auto plan = read_project(cwd, target, nullptr, &inspection, expansions, nullptr, native_triple);
    if (!plan) return std::unexpected(std::move(plan.error()));
    resolved = std::move(*plan);
  } catch (const RuleDemand&) {
    // The unresolved fact was recorded where the planner first needed the rule.
    graph.complete = false;
  }
  const auto edge_order = [](const auto& edge) {
    return std::tie(edge.consumer, edge.location.line, edge.location.column, edge.usage,
                    edge.producer, edge.output, edge.binding);
  };
  std::ranges::sort(graph.edges, {}, edge_order);
  const auto value_edge_order = [](const auto& edge) {
    return std::tie(edge.consumer, edge.location.line, edge.location.column, edge.usage,
                    edge.producer, edge.export_name, edge.binding);
  };
  std::ranges::sort(*graph.value_edges, {}, value_edge_order);
  return inspection;
}

std::expected<std::variant<ProjectPlan, RuleDemand>, std::string> plan_with_rules(
    const fs::path& cwd, std::string_view target, std::span<const RuleExpansion> expansions,
    std::optional<Fp128>* definition, ProjectWatchState* watch, std::string_view native_triple) {
  using PlanResult = std::variant<ProjectPlan, RuleDemand>;
  WatchObservations observed;
  auto result = [&]() -> std::expected<PlanResult, std::string> {
    try {
      auto plan = read_project(cwd, target, watch ? &observed : nullptr, nullptr, expansions, definition,
                               native_triple);
      if (!plan) return std::unexpected(std::move(plan.error()));
      return PlanResult(std::move(*plan));
    } catch (RuleDemand& demand) {
      return PlanResult(std::move(demand));
    }
  }();
  if (watch) {
    watch->paths.assign(observed.paths.begin(), observed.paths.end());
    watch->pending.reset();
    if (!result) watch->stamp = fingerprint(std::tie(observed.manifest, observed.values, result.error()));
    else if (const auto* plan = std::get_if<ProjectPlan>(&*result))
      watch->stamp = fingerprint(std::tie(observed.manifest, *plan));
    else {
      const auto& demand = std::get<RuleDemand>(*result);
      watch->stamp = fingerprint(std::tie(observed.manifest, observed.values, demand));
      watch->pending = demand;
    }
  }
  return result;
}

std::expected<RulePackageRef, std::string> capability_package(std::string_view package) {
  try {
    return resolve_capability_package(package, {});
  } catch (PlanningError& error) {
    return std::unexpected(std::move(error.message));
  }
}

std::expected<RulePackagePlan, std::string> plan_rule_package(
    const fs::path& cwd, const RulePackageRef& package, std::string_view sdk_header,
    const RuleToolchain& toolchain, std::vector<std::string>* failed_paths) {
  std::error_code ec;
  const auto root = fs::absolute(cwd, ec).lexically_normal();
  if (ec) return std::unexpected(std::format("cannot resolve workspace directory: {}", ec.message()));
  WatchObservations observed;
  try {
    const auto lower = [&](const fs::path& directory, std::string_view target, std::string_view text,
                           std::span<const EmbeddedInput> inputs) {
      auto manifest = parse_manifest(text);
      if (!manifest) fail(manifest.error().location, std::move(manifest.error().message));
      auto result = Planner(directory, *manifest, &observed, nullptr).package(target, sdk_header, root, toolchain, inputs);
      result.observed_paths.assign(observed.paths.begin(), observed.paths.end());
      return result;
    };
    if (package.kind == RulePackageRef::Kind::standard) {
      if (package.target != standard_package.target) fail({}, "unknown standard rule package");
      return lower(root, "rules", {standard_manifest, sizeof(standard_manifest) - 1}, standard_inputs);
    }
    if (package.kind == RulePackageRef::Kind::capabilities) {
      if (package.target != capabilities_package.target) fail({}, "unknown capabilities rule package");
      return lower(root, "rules", {capabilities_manifest, sizeof(capabilities_manifest) - 1}, capabilities_inputs);
    }
    const auto target = relative_path(package.target, {});
    const auto directory = root / target.parent_path();
    const auto path = directory / "build.ycxx";
    observed.paths.insert(path_text(path));
    (void)regular_file(path, {});
    return lower(directory, path_text(target.filename()), read_file(path, {}), {});
  } catch (PlanningError& error) {
    if (failed_paths) failed_paths->assign(observed.paths.begin(), observed.paths.end());
    return std::unexpected(std::format("rule package '{}': {}", package.target, error.message));
  }
}

std::expected<RuleInputPlan, std::string> plan_rule_inputs(
    const fs::path& cwd, const RuleDemand& demand, std::span<const RuleExpansion> expansions,
    std::string_view native_triple, std::vector<std::string>* paths) {
  if (paths) paths->clear();
  std::error_code ec;
  const auto root = fs::absolute(cwd, ec).lexically_normal();
  if (ec) return std::unexpected(std::format("cannot resolve workspace directory: {}", ec.message()));
  WatchObservations observed;
  std::expected<RuleInputPlan, std::string> result;
  try {
    auto* watch = paths ? &observed : nullptr;
    const auto manifest = read_workspace_manifest(root, watch);
    result = Planner(root, manifest, watch, nullptr, expansions, native_triple).planning_inputs(demand);
  } catch (PlanningError& error) {
    result = std::unexpected(std::move(error.message));
  } catch (const RuleDemand& producer) {
    result = std::unexpected(std::format("unresolved planning input producer '{}' for target '{}'",
                                        producer.target, demand.target));
  }
  if (paths) paths->assign(observed.paths.begin(), observed.paths.end());
  return result;
}

std::expected<std::variant<RuleWitnessPlan, RuleDemand>, std::string> plan_rule_witness(
    const fs::path& cwd, const RuleExpansion& expansion, std::span<const RuleExpansion> expansions,
    std::string_view native_triple, std::vector<std::string>* paths) {
  if (paths) paths->clear();
  std::error_code ec;
  const auto root = fs::absolute(cwd, ec).lexically_normal();
  if (ec) return std::unexpected(std::format("cannot resolve workspace directory: {}", ec.message()));
  WatchObservations observed;
  using PlanResult = std::variant<RuleWitnessPlan, RuleDemand>;
  std::expected<PlanResult, std::string> result;
  try {
    auto* watch = paths ? &observed : nullptr;
    const auto manifest = read_workspace_manifest(root, watch);
    result = PlanResult(Planner(root, manifest, watch, nullptr, expansions, native_triple).witness(expansion));
  } catch (PlanningError& error) {
    result = std::unexpected(std::move(error.message));
  } catch (RuleDemand& demand) {
    result = PlanResult(std::move(demand));
  }
  if (paths) paths->assign(observed.paths.begin(), observed.paths.end());
  return result;
}

}  // namespace ycxx::build
