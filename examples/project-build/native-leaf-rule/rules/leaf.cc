#include "ycxx_rule_api.h"

namespace {

bool equal(const char* text, const char* expected) {
  if (!text) return false;
  while (*text == *expected) {
    if (*text == '\0') return true;
    ++text;
    ++expected;
  }
  return false;
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!host || host->abi_version != YCXX_RULE_ABI_VERSION || host->struct_size < sizeof(*host) ||
      !host->emit) return 1;

  if (equal(host->rule, "header") && equal(host->target, "score")) {
    const auto& definition = R"ycxx(action.generate "score" {
    tool = "/bin/sh"
    args = ["-c", "printf 'inline constexpr int leaf_score = 17;\\n' > \"$1\"",
            "leaf-header", output("header")]
    outputs = { header = "leaf_score.hh" }
}
)ycxx";
    return host->emit(host->context, definition, sizeof(definition) - 1);
  }

  if (equal(host->rule, "executable") && equal(host->target, "app")) {
    const auto& definition = R"ycxx(native.executable "app" {
    sources = ["main.cc"]
    generated_headers = [artifact("score", "header")]
}
)ycxx";
    return host->emit(host->context, definition, sizeof(definition) - 1);
  }

  const auto& message = "expected leaf.header \"score\" or leaf.executable \"app\"";
  if (host->diagnose) host->diagnose(host->context, message, sizeof(message) - 1);
  return 1;
}
