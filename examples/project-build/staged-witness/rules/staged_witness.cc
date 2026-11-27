#include "ycxx_rule_api.h"

namespace {

constexpr char schema[] = "example.score.v1";
constexpr uint32_t max_score = 255;

constexpr char runner[] = R"ycxx(action.generate "runner" {
    tool = "/bin/cp"
    args = ["-p", input("script"), output("runner")]
    inputs = { script = "analyzer.sh" }
    outputs = { runner = "analyzer.sh" }
}
)ycxx";

constexpr char observe[] = R"ycxx(action.observe "observed" {
    tool = input("analyzer")
    inputs = { analyzer = artifact("runner", "runner") }
    export = "score"
    schema = "example.score.v1"
}
)ycxx";

constexpr char header_start[] = R"ycxx(action.generate "config" {
    tool = "/bin/sh"
    args = ["-c", "printf '#pragma once\\ninline constexpr int generated_score = %s;\\n' \"$1\" > \"$2\"",
            "score-header", ")ycxx";
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

bool valid_host(const ycxx_rule_host_v1* host) {
  return host && host->abi_version == YCXX_RULE_ABI_VERSION && host->struct_size >= sizeof(*host);
}

template<size_t Size>
int reject(const ycxx_rule_host_v1* host, const char (&message)[Size]) {
  if (host->diagnose) host->diagnose(host->context, message, Size - 1);
  return 1;
}

bool space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool parse_score(ycxx_rule_bytes_v1 out, uint32_t& score) {
  size_t first = 0;
  size_t end = out.size;
  while (first < end && space(out.data[first])) ++first;
  while (first < end && space(out.data[end - 1])) --end;
  if (first == end) return false;
  for (size_t pos = first; pos < end; ++pos) {
    const char c = out.data[pos];
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<uint32_t>(c - '0');
    if (score > (max_score - digit) / 10) return false;
    score = score * 10 + digit;
  }
  return true;
}

void append(char*& next, const char* text) {
  while (*text != '\0') *next++ = *text++;
}

int emit_header(const ycxx_rule_host_v1* host) {
  if (host->fact_count != 1 || !host->facts) return reject(host, "expected the score fact");
  const auto& fact = host->facts[0];
  if (!equal(fact.name, "score") || !equal(fact.schema, schema) || fact.row_count != 1 ||
      !equal(fact.rows[0].name, "score") || fact.rows[0].field_count != 1)
    return reject(host, "expected one score row in example.score.v1");
  const auto& value = fact.rows[0].fields[0];
  if (!equal(value.name, "value") || value.kind != YCXX_RULE_FACT_U32_V1 || value.number > max_score)
    return reject(host, "expected a score value from 0 to 255");

  char digits[4] = {};
  size_t start = 3;
  uint32_t score = value.number;
  do {
    digits[--start] = static_cast<char>('0' + score % 10);
    score /= 10;
  } while (score != 0);

  char definition[sizeof(header_start) + sizeof(digits) + sizeof(header_end)];
  char* next = definition;
  append(next, header_start);
  append(next, digits + start);
  append(next, header_end);
  return host->emit(host->context, definition, next - definition);
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!valid_host(host) || !host->emit) return 1;
  if (equal(host->rule, "runner") && equal(host->target, "runner"))
    return host->emit(host->context, runner, sizeof(runner) - 1);
  if (equal(host->rule, "observe") && equal(host->target, "observed"))
    return host->emit(host->context, observe, sizeof(observe) - 1);
  if (equal(host->rule, "header") && equal(host->target, "config")) return emit_header(host);
  return reject(host, "expected the runner, observed, or config target");
}

extern "C" int ycxx_rule_classify_v1(const ycxx_rule_host_v1* host) {
  if (!valid_host(host) || !host->witness || !host->export_fact) return 1;
  const auto& witness = *host->witness;
  if (!equal(witness.name, "score") || !equal(witness.schema, schema))
    return reject(host, "expected the score witness");
  if (witness.exit_status != 0) return reject(host, "the score analyzer exited unsuccessfully");
  uint32_t score = 0;
  if (!parse_score(witness.out, score)) return reject(host, "expected a decimal score from 0 to 255");

  const ycxx_rule_fact_field_v1 value{"value", YCXX_RULE_FACT_U32_V1, score, {}};
  const ycxx_rule_fact_row_v1 row{"score", &value, 1};
  const ycxx_rule_fact_v1 fact{witness.name, schema, &row, 1};
  return host->export_fact(host->context, &fact);
}
