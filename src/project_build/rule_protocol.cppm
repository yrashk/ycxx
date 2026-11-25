// Values exchanged between a build coordinator and a rule worker (#1257).
export module ycxx.build.rule_protocol;

import std;

export namespace ycxx::build {

struct RuleAttribute {
  std::string name;
  std::string value;
  bool operator==(const RuleAttribute&) const = default;
};

struct RuleDemand {
  std::string target;
  std::string rule;
  std::string package;
  std::vector<RuleAttribute> attributes;
  std::vector<std::string> reads;
  bool operator==(const RuleDemand&) const = default;
};

struct RuleExpansion {
  RuleDemand demand;
  std::string definition;
};

struct RuleRead {
  std::string path;
  std::optional<std::string> staged;
};

struct RuleWorkerRequest {
  int version = 1;
  std::string target;
  std::string rule;
  std::vector<RuleAttribute> attributes;
  std::vector<RuleRead> reads;
};

}  // namespace ycxx::build
