// Values exchanged between a build coordinator and a rule worker (#1257).
export module ycxx.build.rule_protocol;

import std;
import ycxx.build.inspection_model;

export namespace ycxx::build {

struct RuleAttribute {
  std::string name;
  std::string value;
  bool operator==(const RuleAttribute&) const = default;
};

struct RuleCapabilityBinding {
  std::string name;
  std::string package;
  std::string probe;
  bool operator==(const RuleCapabilityBinding&) const = default;
};

struct RulePackageRef {
  enum class Kind : std::uint8_t { workspace, standard, capabilities };
  Kind kind = Kind::workspace;
  std::string target;
  bool operator==(const RulePackageRef&) const = default;
};

struct RuleDemand {
  std::string target;
  std::string rule;
  RulePackageRef package;
  std::string definition;  // normalized record of the rule arguments
  std::vector<RuleAttribute> attributes;
  std::vector<std::string> reads;
  std::vector<RuleCapabilityBinding> capabilities = {};
  bool operator==(const RuleDemand&) const = default;
};

struct RuleExpansion {
  RuleDemand demand;
  std::string definition;
  std::vector<PlanFact> capability_facts = {};
};

struct RuleRead {
  std::string path;
  std::optional<std::string> staged;
};

struct RuleCapabilityValue {
  std::string name;
  std::uint32_t support;
};

struct RuleWorkerRequest {
  int version = 1;
  std::string target;
  std::string rule;
  std::string definition;
  std::vector<RuleAttribute> attributes;
  std::vector<RuleRead> reads;
  std::optional<std::vector<RuleCapabilityValue>> capabilities = std::nullopt;
};

}  // namespace ycxx::build
