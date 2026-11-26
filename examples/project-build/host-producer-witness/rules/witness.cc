#include "ycxx_rule_api.h"

namespace {

constexpr char schema[] = "example.score.v1";

constexpr char observe[] = R"ycxx(action.observe "observed" {
    tool = input("analyzer")
    inputs = { analyzer = artifact("analyzer", "executable") }
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

const char* score_spelling(uint32_t score) {
  if (score == 17) return "17";
  if (score == 23) return "23";
  return nullptr;
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
  const char* score = score_spelling(value.number);
  if (!equal(value.name, "value") || value.kind != YCXX_RULE_FACT_U32_V1 || !score)
    return reject(host, "expected score 17 or 23");

  char definition[sizeof(header_start) + sizeof("23") + sizeof(header_end)];
  char* next = definition;
  append(next, header_start);
  append(next, score);
  append(next, header_end);
  return host->emit(host->context, definition, next - definition);
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!valid_host(host) || !host->emit) return 1;
  if (equal(host->rule, "observe") && equal(host->target, "observed"))
    return host->emit(host->context, observe, sizeof(observe) - 1);
  if (equal(host->rule, "header") && equal(host->target, "config")) return emit_header(host);
  return reject(host, "expected the observed or config target");
}

extern "C" int ycxx_rule_classify_v1(const ycxx_rule_host_v1* host) {
  if (!valid_host(host) || !host->witness || !host->export_fact) return 1;
  const auto& witness = *host->witness;
  if (!equal(witness.name, "score") || !equal(witness.schema, schema))
    return reject(host, "expected the score witness");
  const auto score = static_cast<uint32_t>(witness.exit_status);
  if (!score_spelling(score)) return reject(host, "expected the analyzer to exit with status 17 or 23");

  const ycxx_rule_fact_field_v1 value{"value", YCXX_RULE_FACT_U32_V1, score, {}};
  const ycxx_rule_fact_row_v1 row{"score", &value, 1};
  const ycxx_rule_fact_v1 fact{witness.name, schema, &row, 1};
  return host->export_fact(host->context, &fact);
}
