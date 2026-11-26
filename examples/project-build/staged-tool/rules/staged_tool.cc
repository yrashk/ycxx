#include "ycxx_rule_api.h"

namespace {

constexpr char runner[] = R"ycxx(action.generate "runner" {
    tool = "/bin/cp"
    args = ["-p", input("script"), output("runner")]
    inputs = { script = "runner.sh" }
    outputs = { runner = "runner.sh" }
}
)ycxx";

constexpr char header[] = R"ycxx(action.generate "config" {
    tool = input("runner")
    args = [output("header")]
    inputs = { runner = artifact("runner", "runner") }
    outputs = { header = "generated_config.hh" }
}
)ycxx";

bool equal(const char* left, const char* right) {
  if (!left) return false;
  while (*left == *right) {
    if (*left == '\0') return true;
    ++left;
    ++right;
  }
  return false;
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!host || host->abi_version != YCXX_RULE_ABI_VERSION || host->struct_size < sizeof(*host) ||
      !host->emit) return 1;
  if (equal(host->rule, "runner") && equal(host->target, "runner"))
    return host->emit(host->context, runner, sizeof(runner) - 1);
  if (equal(host->rule, "header") && equal(host->target, "config"))
    return host->emit(host->context, header, sizeof(header) - 1);
  constexpr char message[] = "expected runner rule for target 'runner' or header rule for target 'config'";
  if (host->diagnose) host->diagnose(host->context, message, sizeof(message) - 1);
  return 1;
}
