#pragma once

#include "ycxx_rule_api.h"

namespace {

using Text = ycxx_rule_bytes_v1;

Text text(const char* data) {
  size_t size = 0;
  while (data[size] != '\0') ++size;
  return {data, size};
}

bool equal(Text left, const char* right) {
  size_t i = 0;
  while (i < left.size && right[i] != '\0' && left.data[i] == right[i]) ++i;
  return i == left.size && right[i] == '\0';
}

bool digit(char c) { return c >= '0' && c <= '9'; }

// Parse the witness as JSON, including fields outside the descriptor projection.
// Strings are borrowed without decoding escapes. The projected keys and GLSL
// identifiers are plain ASCII in spirv-cross output.
struct Json {
  Text input;
  size_t at = 0;
};

char peek(const Json& json) { return json.at < json.input.size ? json.input.data[json.at] : '\0'; }

void space(Json& json) {
  while (peek(json) == ' ' || peek(json) == '\n' || peek(json) == '\r' || peek(json) == '\t') ++json.at;
}

bool take(Json& json, char token) {
  space(json);
  if (peek(json) != token) return false;
  ++json.at;
  return true;
}

bool string(Json& json, Text& result) {
  if (!take(json, '"')) return false;
  const size_t start = json.at;
  while (peek(json) != '"') {
    const char c = peek(json);
    if (static_cast<unsigned char>(c) < 0x20) return false;
    ++json.at;
    if (c != '\\') continue;
    const char escape = peek(json);
    if (escape == '\0') return false;
    ++json.at;
    if (escape == 'u') {
      for (size_t i = 0; i < 4; ++i) {
        const char hex = peek(json);
        if (!digit(hex) && !(hex >= 'a' && hex <= 'f') && !(hex >= 'A' && hex <= 'F'))
          return false;
        ++json.at;
      }
    } else if (escape != '"' && escape != '\\' && escape != '/' && escape != 'b' &&
               escape != 'f' && escape != 'n' && escape != 'r' && escape != 't') {
      return false;
    }
  }
  result = {json.input.data + start, json.at - start};
  ++json.at;
  return true;
}

bool digits(Json& json) {
  const size_t start = json.at;
  while (digit(peek(json))) ++json.at;
  return json.at != start;
}

bool integer(Json& json) {
  if (!digit(peek(json))) return false;
  if (peek(json) == '0') {
    ++json.at;
    return !digit(peek(json));
  }
  return digits(json);
}

bool u32(Json& json, uint32_t& result) {
  space(json);
  const size_t start = json.at;
  if (!integer(json)) return false;
  result = 0;
  for (size_t i = start; i < json.at; ++i) {
    const auto next = static_cast<uint32_t>(json.input.data[i] - '0');
    if (result > (0xffffffffu - next) / 10u) return false;
    result = result * 10u + next;
  }
  return true;
}

bool number(Json& json) {
  if (peek(json) == '-') ++json.at;
  if (!integer(json)) return false;
  if (peek(json) == '.') {
    ++json.at;
    if (!digits(json)) return false;
  }
  if (peek(json) == 'e' || peek(json) == 'E') {
    ++json.at;
    if (peek(json) == '+' || peek(json) == '-') ++json.at;
    return digits(json);
  }
  return true;
}

bool literal(Json& json, const char* spelling) {
  while (*spelling != '\0') {
    if (peek(json) != *spelling++) return false;
    ++json.at;
  }
  return true;
}

bool skip(Json& json, unsigned depth = 0) {
  if (depth == 32) return false;
  space(json);
  Text ignored{};
  switch (peek(json)) {
    case '{':
      ++json.at;
      if (take(json, '}')) return true;
      do {
        if (!string(json, ignored) || !take(json, ':') || !skip(json, depth + 1)) return false;
      } while (take(json, ','));
      return take(json, '}');
    case '[':
      ++json.at;
      if (take(json, ']')) return true;
      do {
        if (!skip(json, depth + 1)) return false;
      } while (take(json, ','));
      return take(json, ']');
    case '"': return string(json, ignored);
    case 't': return literal(json, "true");
    case 'f': return literal(json, "false");
    case 'n': return literal(json, "null");
    default: return number(json);
  }
}

bool done(Json& json) {
  space(json);
  return json.at == json.input.size;
}

}  // namespace
