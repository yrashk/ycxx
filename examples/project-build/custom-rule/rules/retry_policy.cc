#include "ycxx_rule_api.h"

namespace {

constexpr char action[] = R"ycxx(" {
    tool = "/bin/sh"
    args = ["-c", "printf '#pragma once\\ninline constexpr int retry_limit = %s;\\n' \"$1\" > \"$2\"",
            "retry-header", ")ycxx";
constexpr char outputs[] = R"ycxx(", output("header")]
    outputs = { header = "retry_limit.hh" }
}
)ycxx";

bool equal(const char* left, const char* right) {
  while (*left == *right) {
    if (*left == '\0') return true;
    ++left;
    ++right;
  }
  return false;
}

bool space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool target_character(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

template<size_t Size>
int reject(const ycxx_rule_host_v1* host, const char (&message)[Size]) {
  if (host->diagnose) host->diagnose(host->context, message, Size - 1);
  return 1;
}

void append(char*& cursor, const char* text) {
  while (*text != '\0') *cursor++ = *text++;
}

int emit_header(const ycxx_rule_host_v1* host, char limit) {
  constexpr char prefix[] = "action.generate \"";
  constexpr size_t max_target_size = 63;
  char definition[sizeof(prefix) + max_target_size + sizeof(action) + 1 + sizeof(outputs)];
  char* cursor = definition;
  append(cursor, prefix);
  const char* target = host->target;
  size_t length = 0;
  while (length < max_target_size && target_character(target[length])) {
    *cursor++ = target[length++];
  }
  if (length == 0 || target[length] != '\0')
    return reject(host, "expected a target name of 1 to 63 ASCII letters, digits, underscores or hyphens");

  // Only the normalized digit enters the action, as a positional argument to the shell.
  append(cursor, action);
  *cursor++ = limit;
  append(cursor, outputs);
  return host->emit(host->context, definition, cursor - definition);
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!host || host->abi_version != YCXX_RULE_ABI_VERSION || host->struct_size < sizeof(*host) ||
      !host->rule || !host->target || !host->read || !host->emit) return 1;
  if (!equal(host->rule, "header")) return reject(host, "expected the header rule");

  ycxx_rule_bytes_v1 settings{};
  if (host->read(host->context, "retry-limit.txt", &settings) != 1)
    return reject(host, "retry-limit.txt must exist and be listed in reads");

  size_t first = 0;
  size_t last = settings.size;
  while (first < last && space(settings.data[first])) ++first;
  while (last > first && space(settings.data[last - 1])) --last;
  if (last - first != 1 || settings.data[first] < '1' || settings.data[first] > '9')
    return reject(host, "retry-limit.txt must contain a single digit from 1 to 9");
  return emit_header(host, settings.data[first]);
}
