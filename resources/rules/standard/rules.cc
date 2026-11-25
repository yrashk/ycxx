#include "ycxx_rule_api.h"

namespace {

// A worker invokes the rule once and copies emitted bytes before returning.
constexpr size_t buffer_size = 1024 * 1024;
char buffer[buffer_size];

bool equal(const char* left, const char* right) {
  size_t i = 0;
  while (left[i] == right[i]) {
    if (left[i] == 0) return true;
    ++i;
  }
  return false;
}

bool append(size_t* used, const char* data, size_t size) {
  if (size > buffer_size - *used) return false;
  for (size_t i = 0; i < size; ++i) buffer[*used + i] = data[i];
  *used += size;
  return true;
}

bool append_name(size_t* used, const char* name) {
  if (name[0] == 0 || !append(used, "\"", 1)) return false;
  for (size_t i = 0; name[i] != 0; ++i) {
    const char c = name[i];
    const char* escape = nullptr;
    switch (c) {
      case '"': escape = "\\\""; break;
      case '\\': escape = "\\\\"; break;
      case '\n': escape = "\\n"; break;
      case '\r': escape = "\\r"; break;
      case '\t': escape = "\\t"; break;
      default: if (static_cast<unsigned char>(c) < 0x20) return false; break;
    }
    if (escape) {
      if (!append(used, escape, 2)) return false;
    } else if (!append(used, name + i, 1)) {
      return false;
    }
  }
  return append(used, "\"", 1);
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!host || host->abi_version != YCXX_RULE_ABI_VERSION ||
      host->struct_size < sizeof(*host) || !host->rule || !host->target ||
      !host->definition.data || !host->emit) return 1;

  const char* record = host->definition.data;
  const size_t size = host->definition.size;
  size_t used = 0;
  if (equal(host->rule, "executable")) {
    if (!append(&used, "native.executable ", sizeof("native.executable ") - 1)) return 3;
  } else if (equal(host->rule, "library")) {
    if (!append(&used, "native.library ", sizeof("native.library ") - 1)) return 3;
  } else if (equal(host->rule, "generate")) {
    if (!append(&used, "action.generate ", sizeof("action.generate ") - 1)) return 3;
  } else {
    return 2;
  }

  if (size < 2 || record[0] != '{' || record[size - 1] != '}' ||
      !append_name(&used, host->target) || !append(&used, " ", 1) ||
      !append(&used, record, size)) return 3;
  return host->emit(host->context, buffer, used);
}
