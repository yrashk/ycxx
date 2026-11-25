#include "ycxx_rule_api.h"

namespace {

using Text = ycxx_rule_bytes_v1;

// The header and filename travel as arguments to /bin/sh. Leave room below supported
// hosts' argument limits. The worker invokes the rule once and copies emitted bytes.
char header_bytes[64 * 1024];
char definition_bytes[1024 * 1024];

enum RuleError {
  incompatible_host = 1,
  unknown_rule = 2,
  invalid_namespace = 3,
  invalid_filename = 4,
  invalid_capability = 5,
  oversized_definition = 6,
};

template<size_t Size>
constexpr Text literal(const char (&text)[Size]) {
  return {text, Size - 1};
}

Text c_string(const char* text) {
  size_t size = 0;
  while (text[size] != 0) ++size;
  return {text, size};
}

int compare(Text left, Text right) {
  for (size_t i = 0; i < left.size && i < right.size; ++i) {
    if (left.data[i] != right.data[i]) return left.data[i] < right.data[i] ? -1 : 1;
  }
  if (left.size == right.size) return 0;
  return left.size < right.size ? -1 : 1;
}

bool letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// [lex.key] and the alternative operator tokens from [lex.digraph].
constexpr char keywords[] =
    "alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t "
    "char16_t char32_t class compl concept const const_cast consteval constexpr constinit "
    "continue contract_assert co_await co_return co_yield decltype default delete do double "
    "dynamic_cast else enum explicit export extern false float for friend goto if inline int "
    "long mutable namespace new noexcept not not_eq nullptr operator or or_eq private "
    "protected public register reinterpret_cast requires return short signed sizeof static "
    "static_assert static_cast struct switch template this thread_local throw true try "
    "typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq ";

bool valid_identifier(Text name) {
  // Use the portable ASCII subset of C++ identifiers for generated declarations.
  if (name.size == 0 || (!letter(name.data[0]) && name.data[0] != '_')) return false;
  if (name.size > 1 && name.data[0] == '_' && name.data[1] >= 'A' && name.data[1] <= 'Z')
    return false;
  for (size_t i = 1; i < name.size; ++i) {
    const char c = name.data[i];
    if (c == '_' && name.data[i - 1] == '_') return false;
    if (!letter(c) && c != '_' && !(c >= '0' && c <= '9')) return false;
  }
  size_t begin = 0;
  for (size_t i = 0; keywords[i] != 0; ++i) {
    if (keywords[i] != ' ') continue;
    const Text keyword{keywords + begin, i - begin};
    if (compare(name, keyword) == 0) return false;
    begin = i + 1;
  }
  return true;
}

bool valid_namespace(Text name) {
  size_t begin = 0;
  for (size_t end = 0; ; ++end) {
    if (end < name.size && name.data[end] != ':') continue;
    const Text component{name.data + begin, end - begin};
    if (!valid_identifier(component)) return false;
    // The first component is declared in the global namespace ([lex.name], [namespace.std]).
    if (begin == 0 && (component.data[0] == '_' || compare(component, literal("std")) == 0))
      return false;
    if (end == name.size) return true;
    if (end + 1 == name.size || name.data[end + 1] != ':') return false;
    ++end;
    begin = end + 1;
  }
}

bool valid_filename(Text filename) {
  if (filename.size == 0 || compare(filename, literal(".")) == 0 ||
      compare(filename, literal("..")) == 0) return false;
  for (size_t i = 0; i < filename.size; ++i) {
    const char c = filename.data[i];
    if (c == '/' || (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\r' && c != '\t'))
      return false;
  }
  return true;
}

bool attribute(const ycxx_rule_host_v1* host, const char* key, Text& value) {
  Text specified{};
  const int found = host->attribute(host->context, key, &specified);
  if (found == 0) return true;
  if (found != 1 || !specified.data) return false;
  value = specified;
  return true;
}

struct Buffer {
  char* data;
  size_t capacity;
  size_t used = 0;
};

bool append(Buffer& buffer, Text text) {
  if (text.size > buffer.capacity - buffer.used) return false;
  for (size_t i = 0; i < text.size; ++i) buffer.data[buffer.used + i] = text.data[i];
  buffer.used += text.size;
  return true;
}

bool append(Buffer& buffer, const char* text) {
  return append(buffer, c_string(text));
}

bool quote(Buffer& buffer, Text text) {
  if (!append(buffer, "\"")) return false;
  for (size_t i = 0; i < text.size; ++i) {
    const char c = text.data[i];
    Text escaped{text.data + i, 1};
    switch (c) {
      case '"': escaped = literal("\\\""); break;
      case '\\': escaped = literal("\\\\"); break;
      case '\n': escaped = literal("\\n"); break;
      case '\r': escaped = literal("\\r"); break;
      case '\t': escaped = literal("\\t"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) return false;
        break;
    }
    if (!append(buffer, escaped)) return false;
  }
  return append(buffer, "\"");
}

Text support_name(uint32_t support) {
  switch (support) {
    case YCXX_RULE_SUPPORTED_V1: return literal("supported");
    case YCXX_RULE_UNSUPPORTED_V1: return literal("unsupported");
    case YCXX_RULE_INDETERMINATE_V1: return literal("indeterminate");
    default: return {};
  }
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!host || host->abi_version != YCXX_RULE_ABI_VERSION || host->struct_size < sizeof(*host) ||
      !host->rule || !host->target || !*host->target || !host->attribute || !host->emit ||
      (host->capability_count != 0 && !host->capabilities)) return incompatible_host;
  if (compare(c_string(host->rule), literal("header")) != 0) return unknown_rule;

  Text scope = literal("ycxx_capabilities");
  Text filename = literal("compiler_profile.hh");
  if (!attribute(host, "namespace", scope) || !valid_namespace(scope)) return invalid_namespace;
  if (!attribute(host, "filename", filename) || !valid_filename(filename)) return invalid_filename;

  Buffer header{header_bytes, sizeof header_bytes};
  if (!append(header, "#pragma once\n\nnamespace ") || !append(header, scope) ||
      !append(header, " {\n\n"
                      "enum class support { supported, unsupported, indeterminate };\n\n"
                      "template<support Value>\n"
                      "inline constexpr bool available = Value == support::supported;\n\n"))
    return oversized_definition;

  Text previous{};
  for (size_t i = 0; i < host->capability_count; ++i) {
    const auto& capability = host->capabilities[i];
    if (!capability.name) return invalid_capability;
    const Text name = c_string(capability.name);
    const Text state = support_name(capability.support);
    // The worker supplies names in order. Strict ordering also rules out duplicates.
    if (!valid_identifier(name) || !state.data || compare(previous, name) >= 0 ||
        compare(name, literal("support")) == 0 || compare(name, literal("available")) == 0)
      return invalid_capability;
    if (!append(header, "inline constexpr support ") || !append(header, name) ||
        !append(header, " = support::") || !append(header, state) || !append(header, ";\n"))
      return oversized_definition;
    previous = name;
  }
  if (!append(header, "\n}  // namespace ") || !append(header, scope) || !append(header, "\n"))
    return oversized_definition;
  if (filename.size > header.capacity - header.used) return oversized_definition;

  // The command is fixed. The decoded header and output path are positional arguments.
  Buffer definition{definition_bytes, sizeof definition_bytes};
  const Text contents{header.data, header.used};
  if (!append(definition, "action.generate ") || !quote(definition, c_string(host->target)) ||
      !append(definition, " { tool = \"/bin/sh\" args = [\"-c\", ") ||
      !quote(definition, literal("printf '%s' \"$1\" > \"$2\"")) ||
      !append(definition, ", \"capability-header\", ") ||
      !quote(definition, contents) ||
      !append(definition, ", output(\"header\")] outputs = { header = ") ||
      !quote(definition, filename) || !append(definition, " } }\n")) return oversized_definition;
  return host->emit(host->context, definition.data, definition.used);
}
