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

struct RuleValueReference {
  std::string target;
  std::string field;
  bool operator==(const RuleValueReference&) const = default;
};

struct RuleFactReference {
  std::string name;
  RuleValueReference value;
  bool operator==(const RuleFactReference&) const = default;
};

struct RulePlanInput {
  std::string name;
  std::string target;
  std::string output;
  bool operator==(const RulePlanInput&) const = default;
};

struct RuleCapabilityBinding {
  std::string name;
  std::string package;
  std::string probe;
  std::optional<RuleValueReference> value = std::nullopt;
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
  std::optional<std::string> value_export = std::nullopt;
  std::vector<RuleFactReference> facts = {};
  std::vector<RulePlanInput> plan_inputs = {};
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

struct RuleFactField {
  std::string name;
  // A field has exactly one of these values.
  std::optional<std::uint32_t> number = std::nullopt;
  std::optional<std::string> text = std::nullopt;
  bool operator==(const RuleFactField&) const = default;
};

struct RuleFactRow {
  std::string name;
  std::vector<RuleFactField> fields;
  bool operator==(const RuleFactRow&) const = default;
};

struct RuleFactValue {
  std::string schema;
  std::vector<RuleFactRow> rows;
  bool operator==(const RuleFactValue&) const = default;
};

struct RuleFactBinding {
  std::string name;
  RuleFactValue value;
  bool operator==(const RuleFactBinding&) const = default;
};

inline constexpr std::size_t rule_witness_stream_limit = 64 * 1024;

struct RuleWitnessReport {
  std::string name;
  int exit_status;
  std::string out;
  std::string err;
  std::optional<std::string> schema = std::nullopt;
};

struct RuleWorkerRequest {
  int version = 1;
  std::string target;
  std::string rule;
  std::string definition;
  std::vector<RuleAttribute> attributes;
  std::vector<RuleRead> reads;
  std::optional<std::vector<RuleCapabilityValue>> capabilities = std::nullopt;
  std::optional<RuleWitnessReport> witness = std::nullopt;
  std::optional<std::vector<RuleFactBinding>> facts = std::nullopt;
};

}  // namespace ycxx::build
