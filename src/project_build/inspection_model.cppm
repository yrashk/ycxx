// Resolved plan values and their explanation, shared by the inspector formats (#1256).
export module ycxx.build.inspection_model;

import std;

export namespace ycxx::build {

inline constexpr std::string_view unresolved_fact_kind = "unresolved";

struct PlanFact {
  std::string kind;
  std::string target;
  std::string name;
  std::string value;
  std::string origin;
  std::size_t line = 0;
  std::size_t column = 0;
};

struct ModuleSourceLocation {
  std::string path;
  std::uint32_t line;
  std::uint32_t column;
};

struct ModuleDeclarationInspection {
  std::size_t directive_index;
  std::string kind;
  std::string name;
  ModuleSourceLocation location;
};

struct ModuleUnitConfiguration {
  std::string source;
  std::vector<std::string> include_dirs;
  auto operator<=>(const ModuleUnitConfiguration&) const = default;
};

// Only the build system's virtual storage varies with the graph's native target.
inline std::string logical_module_path(std::string_view path, std::string_view native_target) {
  const auto prefix = std::format(".ycxx/targets/{}/virtual", native_target);
  if (path.starts_with(prefix) && (path.size() == prefix.size() || path[prefix.size()] == '/'))
    return std::format(".ycxx/virtual{}", path.substr(prefix.size()));
  return std::string(path);
}

struct ModuleUnitInspection {
  std::size_t id;
  std::string source;
  std::string scan;
  std::vector<ModuleDeclarationInspection> declarations;
  std::optional<ModuleUnitConfiguration> configuration = std::nullopt;
};

struct ModuleCandidateInspection {
  std::size_t unit_id;
  std::size_t directive_index;
};

struct ModuleImportInspection {
  std::size_t unit_id;
  std::size_t directive_index;
  std::string kind;
  std::string name;
  ModuleSourceLocation location;
  std::string status;
  std::vector<ModuleCandidateInspection> candidates;
};

struct ModuleConflictInspection {
  std::string name;
  std::vector<ModuleCandidateInspection> candidates;
};

struct ModuleGraphInspection {
  std::string native_target;
  bool complete = true;
  std::vector<ModuleUnitInspection> units = {};
  std::vector<ModuleImportInspection> imports = {};
  std::vector<ModuleConflictInspection> conflicts = {};
  std::vector<std::vector<std::size_t>> cycles = {};
};

struct PlanInspection {
  int schema = 1;
  std::string target;
  std::string definition;
  std::vector<PlanFact> facts;
  std::optional<ModuleGraphInspection> module_graph = std::nullopt;
};

struct PlanChange {
  std::string change;  // added, removed, changed
  std::optional<PlanFact> before;
  std::optional<PlanFact> after;
};

struct TargetDeclarationLocation {
  std::string path = "build.ycxx";
  std::size_t line;
  std::size_t column;
};

struct TargetCatalogEntry {
  std::string name;
  std::string kind;
  TargetDeclarationLocation location;
};

struct TargetCatalog {
  int schema = 1;
  std::vector<TargetCatalogEntry> targets = {};
};

}  // namespace ycxx::build
