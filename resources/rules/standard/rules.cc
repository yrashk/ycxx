#include "ycxx_rule_api.h"

#include <string>
#include <string_view>

namespace {

// Quote a name using the same escapes as build.ycxx strings.
bool append_name(std::string& definition, std::string_view name) {
  if (name.empty()) return false;
  definition += '"';
  for (const char c : name) {
    switch (c) {
      case '"': definition += "\\\""; break;
      case '\\': definition += "\\\\"; break;
      case '\n': definition += "\\n"; break;
      case '\r': definition += "\\r"; break;
      case '\t': definition += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) return false;
        definition += c;
        break;
    }
  }
  definition += '"';
  return true;
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) try {
  if (!host || host->abi_version != YCXX_RULE_ABI_VERSION ||
      host->struct_size < sizeof(*host) || !host->rule || !host->target ||
      !host->definition.data || !host->emit) return 1;

  const std::string_view rule(host->rule);
  std::string_view primitive;
  if (rule == "executable") primitive = "native.executable";
  else if (rule == "library") primitive = "native.library";
  else if (rule == "generate") primitive = "action.generate";
  else return 2;

  const std::string_view record(host->definition.data, host->definition.size);
  if (record.size() < 2 || record.front() != '{' || record.back() != '}') return 3;
  std::string definition(primitive);
  definition += ' ';
  if (!append_name(definition, host->target)) return 3;
  definition += ' ';
  definition += record;
  return host->emit(host->context, definition.data(), definition.size());
} catch (...) {
  // Keep host-compiler exceptions on the plugin side of the C ABI.
  return 4;
}
