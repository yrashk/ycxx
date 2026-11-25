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

struct PlanInspection {
  int schema = 1;
  std::string target;
  std::string definition;
  std::vector<PlanFact> facts;
};

struct PlanChange {
  std::string change;  // added, removed, changed
  std::optional<PlanFact> before;
  std::optional<PlanFact> after;
};

}  // namespace ycxx::build
