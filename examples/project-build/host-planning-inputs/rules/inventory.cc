#include "ycxx_rule_api.h"

namespace {

constexpr char header_start[] = R"ycxx(action.generate "config" {
    tool = "/bin/sh"
    args = ["-c", "printf '#pragma once\\ninline constexpr int generated_score = %s;\\n' \"$1\" > \"$2\"",
            "inventory-header", ")ycxx";
constexpr char header_end[] = R"ycxx(", output("header")]
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

template<size_t Size>
bool equal(ycxx_rule_bytes_v1 bytes, const char (&text)[Size]) {
  if (!bytes.data || bytes.size != Size - 1) return false;
  for (size_t i = 0; i < bytes.size; ++i)
    if (bytes.data[i] != text[i]) return false;
  return true;
}

template<size_t Size>
int reject(const ycxx_rule_host_v1* host, const char (&message)[Size]) {
  if (host->diagnose) host->diagnose(host->context, message, Size - 1);
  return 1;
}

void append(char*& next, const char* text) {
  while (*text != '\0') *next++ = *text++;
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!host || host->abi_version != YCXX_RULE_ABI_VERSION || host->struct_size < sizeof(*host) ||
      !host->read || !host->emit) return 1;
  if (!equal(host->rule, "header") || !equal(host->target, "config"))
    return reject(host, "expected the config header target");

  ycxx_rule_bytes_v1 inventory{};
  if (host->read(host->context, "inventory", &inventory) != 1)
    return reject(host, "bind the inventory through plan_inputs");
  const char* score = nullptr;
  if (equal(inventory, "score=17\n")) score = "17";
  else if (equal(inventory, "score=23\n")) score = "23";
  else return reject(host, "expected score=17 or score=23 on one line");

  char definition[sizeof(header_start) + sizeof("23") + sizeof(header_end)];
  char* next = definition;
  append(next, header_start);
  append(next, score);
  append(next, header_end);
  return host->emit(host->context, definition, next - definition);
}
