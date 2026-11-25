#include "ycxx_rule_api.h"

namespace {

constexpr char plan[] = R"ycxx(action.observe "wire" {
    export = "fast-mode"
    tool = "/bin/sh"
    args = [input("check"), input("data")]
    inputs = { check = "rules/check-mode.sh" data = "wire.cfg" }
})ycxx";

bool equal(const char* left, const char* right) {
    if (!left) return false;
    while (*left == *right) {
        if (*left == '\0') return true;
        ++left;
        ++right;
    }
    return false;
}

template<size_t Size>
bool starts_with(ycxx_rule_bytes_v1 bytes, const char (&prefix)[Size]) {
    if (bytes.size < Size - 1) return false;
    for (size_t i = 0; i < Size - 1; ++i) {
        if (bytes.data[i] != prefix[i]) return false;
    }
    return true;
}

bool valid_host(const ycxx_rule_host_v1* host) {
    return host && host->abi_version == YCXX_RULE_ABI_VERSION && host->struct_size >= sizeof(*host);
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
    if (!valid_host(host) || !host->emit || !equal(host->rule, "profile") ||
        !equal(host->target, "wire")) return 1;
    return host->emit(host->context, plan, sizeof(plan) - 1);
}

extern "C" int ycxx_rule_classify_v1(const ycxx_rule_host_v1* host) {
    if (!valid_host(host) || !host->witness || !host->export_support) return 1;
    const auto& witness = *host->witness;
    if (!equal(witness.name, "fast-mode")) return 1;

    uint32_t support = YCXX_RULE_INDETERMINATE_V1;
    if (starts_with(witness.out, "profile=")) {
        if (witness.exit_status == 0) support = YCXX_RULE_SUPPORTED_V1;
        if (witness.exit_status == 7 && starts_with(witness.err, "fast mode disabled\n")) {
            support = YCXX_RULE_UNSUPPORTED_V1;
        }
    }
    return host->export_support(host->context, witness.name, support);
}
