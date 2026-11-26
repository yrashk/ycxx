// The lexer's and directive-free preprocessor's diagnostics. Each kind carries its severity and
// message as an annotation, so the table is derived by reflection. Diagnostics are values
// (query-engine.md §A1.7): `pp` reports them through its context with `diag_id(kind)`.
//
// Location seam (#34): `pp` runs before items exist, so its diagnostics use
// `Loc{file_level, byte offset in the file}`; the cell that carries them, `pp(file)`, names the file.
// Messages take integer arguments only (DiagArg); `{N}` is argument N in decimal, `{N:c}` a code
// point, `{N:x}` hexadecimal, `{N:k}` a token kind's spelling.
export module ycxx.lex:diag;

import std;
import ycxx.engine;
import :tok;
import :unicode;

export namespace ycxx::lex {

enum class Severity : u8 { error, warning };

// Diagnostic categories the driver can select. A diagnostic without a category is always shown.
enum class Warning : u8 { uncontrolled, multichar, attributes, sized_deallocation, constant_conversion };

struct message {
  Severity severity;
  char text[112];
  Warning warning = Warning::uncontrolled;
};

enum class LexDiag : u32 {
  // [lex.phases]
  invalid_utf8 [[=message{Severity::error, "invalid UTF-8 code unit sequence"}]] = 1,
  unterminated_comment [[=message{Severity::error, "unterminated /* comment"}]],
  // [lex.pptoken]
  stray_character [[=message{Severity::error, "character {0:c} (U+{0:x}) is not allowed here"}]],
  stray_token [[=message{Severity::error, "stray '{0:k}' in program"}]],
  stray_basic_character [[=message{Severity::error, "stray '{0:c}' in program"}]],
  unterminated_char [[=message{Severity::error, "missing terminating ' character"}]],
  unterminated_string [[=message{Severity::error, "missing terminating \" character"}]],
  empty_char [[=message{Severity::error, "empty character literal"}]],
  // [lex.universal.char]
  ucn_not_scalar [[=message{Severity::error, "universal-character-name U+{0:x} is not a Unicode scalar value"}]],
  ucn_basic_or_control [[=message{Severity::error, "universal-character-name U+{0:x} designates a basic or control character outside a literal"}]],
  ucn_unknown_name [[=message{Severity::error, "no Unicode character has this name or alias"}]],
  ucn_incomplete [[=message{Severity::error, "incomplete universal-character-name"}]],
  // [lex.name]
  identifier_not_nfc [[=message{Severity::error, "identifier is not in Normalization Form C"}]],
  identifier_bad_character [[=message{Severity::error, "character {0:c} (U+{0:x}) cannot appear in an identifier"}]],
  // [lex.string] raw strings
  raw_delimiter_too_long [[=message{Severity::error, "raw string delimiter longer than 16 characters"}]],
  raw_delimiter_bad_character [[=message{Severity::error, "invalid character {0:c} in raw string delimiter"}]],
  unterminated_raw_string [[=message{Severity::error, "raw string literal is not terminated"}]],
  // [lex.icon], [lex.fcon], [lex.ext]
  invalid_number [[=message{Severity::error, "invalid preprocessing number: '{0:c}' cannot follow the literal"}]],
  integer_too_large [[=message{Severity::error, "integer literal is too large for any integer type"}]],
  float_out_of_range [[=message{Severity::warning, "floating-point literal is out of the range of its type; it is infinity"}]],
  // [lex.ccon], [lex.string] escapes
  unknown_escape [[=message{Severity::error, "unknown escape sequence '\\{0:c}'"}]],
  empty_escape [[=message{Severity::error, "escape sequence has no digits"}]],
  unterminated_escape [[=message{Severity::error, "missing '}' in delimited escape sequence"}]],
  escape_out_of_range [[=message{Severity::error, "escape sequence value 0x{0:x} is out of range for its character type"}]],
  char_not_single_unit [[=message{Severity::error, "character literal needs more than one code unit in its encoding"}]],
  multichar_with_prefix [[=message{Severity::error, "a multicharacter literal cannot have an encoding prefix"}]],
  multichar [[=message{Severity::warning, "multicharacter literal has an implementation-defined value", Warning::multichar}]],
  multichar_not_single_unit [[=message{Severity::error, "multicharacter literal contains a character that needs more than one code unit"}]],
  // [lex.string] phase 5
  concat_encoding_mismatch [[=message{Severity::error, "adjacent string literals have different encoding prefixes"}]],
  concat_suffix_mismatch [[=message{Severity::error, "adjacent string literals have different ud-suffixes"}]],
  // [cpp]
  directive_unsupported [[=message{Severity::error, "preprocessing directives are not supported yet"}]],
  invalid_directive [[=message{Severity::error, "invalid preprocessing directive"}]],
  extra_tokens [[=message{Severity::warning, "extra tokens at end of preprocessing directive"}]],
  // [cpp.include]
  include_expected [[=message{Severity::error, "expected \"FILENAME\" or <FILENAME>"}]],
  include_not_found [[=message{Severity::error, "file not found"}]],
  include_too_deep [[=message{Severity::error, "#include nested too deeply"}]],
  // [cpp.replace]
  macro_name_missing [[=message{Severity::error, "macro name missing"}]],
  macro_name_not_identifier [[=message{Severity::error, "macro name must be an identifier"}]],
  macro_name_defined [[=message{Severity::error, "'defined' cannot be used as a macro name"}]],
  macro_redefined [[=message{Severity::error, "macro redefined with a different replacement list"}]],
  macro_param_expected [[=message{Severity::error, "expected a macro parameter name, '...' or ')'"}]],
  macro_param_duplicate [[=message{Severity::error, "duplicate macro parameter name"}]],
  va_outside_variadic [[=message{Severity::error, "__VA_ARGS__ and __VA_OPT__ can appear only in a variadic macro's replacement list"}]],
  va_opt_malformed [[=message{Severity::error, "__VA_OPT__ must be followed by a parenthesized replacement, not nested"}]],
  stringize_not_parameter [[=message{Severity::error, "'#' is not followed by a macro parameter"}]],
  paste_at_edge [[=message{Severity::error, "'##' cannot appear at either end of a replacement list"}]],
  paste_invalid [[=message{Severity::error, "pasting does not form a valid preprocessing token"}]],
  stringize_invalid [[=message{Severity::error, "stringizing does not form a valid string literal"}]],
  directive_in_arguments [[=message{Severity::error, "#include and #pragma within macro arguments are not supported"}]],
  invocation_unterminated [[=message{Severity::error, "unterminated function-like macro invocation"}]],
  too_few_arguments [[=message{Severity::error, "too few arguments in a macro invocation"}]],
  too_many_arguments [[=message{Severity::error, "too many arguments in a macro invocation"}]],
  // [cpp.cond]
  cond_unterminated [[=message{Severity::error, "unterminated conditional directive"}]],
  cond_else_without_if [[=message{Severity::error, "#else or #elif without #if"}]],
  cond_endif_without_if [[=message{Severity::error, "#endif without #if"}]],
  cond_else_after_else [[=message{Severity::error, "#else after #else"}]],
  cond_elif_after_else [[=message{Severity::error, "#elif after #else"}]],
  cond_empty [[=message{Severity::error, "#if with no expression"}]],
  cond_bad_token [[=message{Severity::error, "invalid token in a preprocessor expression"}]],
  cond_expected_value [[=message{Severity::error, "expected a value in a preprocessor expression"}]],
  cond_expected_rparen [[=message{Severity::error, "expected ')' in a preprocessor expression"}]],
  cond_expected_lparen [[=message{Severity::error, "expected '(' after the operator"}]],
  cond_expected_colon [[=message{Severity::error, "expected ':' in a preprocessor expression"}]],
  cond_division_by_zero [[=message{Severity::error, "division by zero in a preprocessor expression"}]],
  cond_defined_operand [[=message{Severity::error, "'defined' needs a macro name"}]],
  // [cpp.line], [cpp.error], [cpp.pragma.op], [cpp.module], [cpp.import]
  line_bad_number [[=message{Severity::error, "#line needs a line number from 1 to 2147483647"}]],
  line_bad_name [[=message{Severity::error, "#line's file name must be a plain string literal"}]],
  line_extra [[=message{Severity::error, "extra tokens after #line's operands"}]],
  error_directive [[=message{Severity::error, "#error"}]],
  warning_directive [[=message{Severity::warning, "#warning"}]],
  pragma_operator_malformed [[=message{Severity::error, "_Pragma takes a parenthesized string literal"}]],
  header_unit_unsupported [[=message{Severity::error, "importing a header unit is not supported yet"}]],
  module_name_macro [[=message{Severity::error, "a module name cannot be an object-like macro"}]],
  module_name_end [[=message{Severity::error, "a module name must be followed by ';' or '['"}]],
  // [cpp.pragma]
  pragma_once_in_main [[=message{Severity::warning, "#pragma once in main file"}]],
  // [cpp.embed]
  embed_bad_parameter [[=message{Severity::error, "invalid embed parameter"}]],
  embed_duplicate_parameter [[=message{Severity::error, "duplicate embed parameter"}]],
  embed_unsupported_parameter [[=message{Severity::error, "unsupported embed parameter"}]],
  embed_negative [[=message{Severity::error, "embed limit or offset must be nonnegative"}]],
  embed_cond_operator [[=message{Severity::error, "conditional operator is not permitted in this embed operand"}]],
  embed_parameter_macro [[=message{Severity::error, "standard embed parameter name used as a macro in an embed operand"}]],
  embed_in_arguments [[=message{Severity::error, "#embed within macro arguments is not supported"}]],
  embed_not_found [[=message{Severity::error, "embed resource not found"}]],
};

// A diagnostic as the lexer produces it, before it reaches a query's context.
struct LexDiagnostic {
  u32 offset = 0;  // physical byte offset in the file
  LexDiag kind{};
  std::array<u64, 2> args{};
  bool operator==(const LexDiagnostic&) const = default;
};

// The base of the lexer's range of DiagIds. The front end's table (#43) assigns areas.
inline constexpr u32 lex_diag_base = 0x1000;
// The item of a diagnostic that `pp` reports: the file itself, at a byte offset.
inline constexpr u32 file_level = 0xffffffffu;

constexpr engine::DiagId diag_id(LexDiag d) { return engine::DiagId(lex_diag_base + std::to_underlying(d)); }

consteval auto lex_diag_enumerators() { return std::define_static_array(std::meta::enumerators_of(^^LexDiag)); }

// The kind of a DiagId in the lexer's range.
constexpr std::optional<LexDiag> lex_diag_of(engine::DiagId id) {
  u32 v = std::to_underlying(id);
  std::optional<LexDiag> r;
  template for (constexpr auto e : lex_diag_enumerators()) {
    if (v == lex_diag_base + std::to_underlying([:e:])) r = [:e:];
  }
  return r;
}

constexpr message message_of(LexDiag d) {
  message m{};
  template for (constexpr auto e : lex_diag_enumerators()) {
    if (d == [:e:]) m = std::meta::extract<message>(std::meta::annotations_of_with_type(e, ^^message)[0]);
  }
  return m;
}

constexpr std::string_view diag_name(LexDiag d) {
  std::string_view r;
  template for (constexpr auto e : lex_diag_enumerators()) {
    if (d == [:e:]) r = std::define_static_string(std::meta::identifier_of(e));
  }
  return r;
}

// Renders a message's placeholders with the given arguments.
std::string render(LexDiag d, std::span<const u64> args) {
  message m = message_of(d);
  std::string_view t(m.text);
  std::string out;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i] != '{' || i + 2 >= t.size()) {
      out += t[i];
      continue;
    }
    std::size_t close = t.find('}', i);
    std::string_view spec = t.substr(i + 1, close - i - 1);
    std::size_t n = std::size_t(spec[0] - '0');
    u64 v = n < args.size() ? args[n] : 0;
    char how = spec.size() > 2 ? spec[2] : 'd';
    if (how == 'c') unicode::append_utf8(out, char32_t(v));
    else if (how == 'x') out += std::format("{:04X}", v);
    else if (how == 'k') out += spelling(Tk(v));
    else out += std::format("{}", v);
    i = close;
  }
  return out;
}

}  // namespace ycxx::lex
