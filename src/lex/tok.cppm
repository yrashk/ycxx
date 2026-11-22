// Token kinds and the token record (docs/design/ast.md §8.1). One `Tk` covers preprocessing
// tokens ([lex.pptoken]) and tokens ([lex.token]): the kinds marked pp-only appear in the lexer's
// output and never in a region. A punctuator's spelling is an annotation on its enumerator, and a
// keyword's spelling is its enumerator's name without `kw_`, so the spelling and keyword tables
// are derived by reflection.
export module ycxx.lex:tok;

import std;

export namespace ycxx::lex {

// Conversion-function-ids use the target type for identity ([class.conv.fct]).
inline constexpr std::string_view conversion_function_name = "operator<conversion>";

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The spelling of a punctuator, as an annotation on its kind.
struct spelled {
  char text[5];
};

enum class Tk : u8 {
  eof,         // the end of the stream
  // pp-only kinds (phases 3 and 4)
  pp_number,   // [lex.ppnumber]; phase 6 makes it int_lit or float_lit
  header_name, // [lex.header]; lexed only on request (Lexer::header_name)
  pp_other,    // a character that is no other preprocessing token; ill-formed in phase 6
  // [lex.name], [lex.literal]
  identifier,
  int_lit,
  float_lit,
  char_lit,
  string_lit,
  // [lex.operators]; the alternative tokens of [lex.digraph] have the primary's kind and `alt`
  l_brace [[=spelled{"{"}]],
  r_brace [[=spelled{"}"}]],
  l_square [[=spelled{"["}]],
  r_square [[=spelled{"]"}]],
  l_paren [[=spelled{"("}]],
  r_paren [[=spelled{")"}]],
  l_splice [[=spelled{"[:"}]],
  r_splice [[=spelled{":]"}]],
  semi [[=spelled{";"}]],
  colon [[=spelled{":"}]],
  ellipsis [[=spelled{"..."}]],
  question [[=spelled{"?"}]],
  coloncolon [[=spelled{"::"}]],
  period [[=spelled{"."}]],
  periodstar [[=spelled{".*"}]],
  arrow [[=spelled{"->"}]],
  arrowstar [[=spelled{"->*"}]],
  caretcaret [[=spelled{"^^"}]],
  tilde [[=spelled{"~"}]],
  exclaim [[=spelled{"!"}]],
  plus [[=spelled{"+"}]],
  minus [[=spelled{"-"}]],
  star [[=spelled{"*"}]],
  slash [[=spelled{"/"}]],
  percent [[=spelled{"%"}]],
  caret [[=spelled{"^"}]],
  amp [[=spelled{"&"}]],
  pipe [[=spelled{"|"}]],
  equal [[=spelled{"="}]],
  plusequal [[=spelled{"+="}]],
  minusequal [[=spelled{"-="}]],
  starequal [[=spelled{"*="}]],
  slashequal [[=spelled{"/="}]],
  percentequal [[=spelled{"%="}]],
  caretequal [[=spelled{"^="}]],
  ampequal [[=spelled{"&="}]],
  pipeequal [[=spelled{"|="}]],
  equalequal [[=spelled{"=="}]],
  exclaimequal [[=spelled{"!="}]],
  less [[=spelled{"<"}]],
  greater [[=spelled{">"}]],
  lessequal [[=spelled{"<="}]],
  greaterequal [[=spelled{">="}]],
  spaceship [[=spelled{"<=>"}]],
  ampamp [[=spelled{"&&"}]],
  pipepipe [[=spelled{"||"}]],
  lessless [[=spelled{"<<"}]],
  greatergreater [[=spelled{">>"}]],
  lesslessequal [[=spelled{"<<="}]],
  greatergreaterequal [[=spelled{">>="}]],
  plusplus [[=spelled{"++"}]],
  minusminus [[=spelled{"--"}]],
  comma [[=spelled{","}]],
  // [lex.operators] preprocessing-operator: pp-only, ill-formed in phase 6
  hash [[=spelled{"#"}]],
  hashhash [[=spelled{"##"}]],
  // [lex.key]
  kw_alignas,
  kw_alignof,
  kw_asm,
  kw_auto,
  kw_bool,
  kw_break,
  kw_case,
  kw_catch,
  kw_char,
  kw_char8_t,
  kw_char16_t,
  kw_char32_t,
  kw_class,
  kw_concept,
  kw_const,
  kw_consteval,
  kw_constexpr,
  kw_constinit,
  kw_const_cast,
  kw_continue,
  kw_contract_assert,
  kw_co_await,
  kw_co_return,
  kw_co_yield,
  kw_decltype,
  kw_default,
  kw_delete,
  kw_do,
  kw_double,
  kw_dynamic_cast,
  kw_else,
  kw_enum,
  kw_explicit,
  kw_export,
  kw_extern,
  kw_false,
  kw_float,
  kw_for,
  kw_friend,
  kw_goto,
  kw_if,
  kw_inline,
  kw_int,
  kw_long,
  kw_mutable,
  kw_namespace,
  kw_new,
  kw_noexcept,
  kw_nullptr,
  kw_operator,
  kw_private,
  kw_protected,
  kw_public,
  kw_register,
  kw_reinterpret_cast,
  kw_requires,
  kw_return,
  kw_short,
  kw_signed,
  kw_sizeof,
  kw_static,
  kw_static_assert,
  kw_static_cast,
  kw_struct,
  kw_switch,
  kw_template,
  kw_this,
  kw_thread_local,
  kw_throw,
  kw_true,
  kw_try,
  kw_typedef,
  kw_typeid,
  kw_typename,
  kw_union,
  kw_unsigned,
  kw_using,
  kw_virtual,
  kw_void,
  kw_volatile,
  kw_wchar_t,
  kw_while,
  // GNU pointer qualifiers (docs/EXTENSIONS.md).
  kw___restrict,
  kw___restrict__,
  embed,  // a phase-4 resource, unpacked when materializing parser tokens
};

// Token flags. `line_start` and `space_before` describe whitespace and are not part of an item's
// identity (§B1.2); the others are.
namespace tok_flag {
inline constexpr u8 line_start = 1;    // first token of a logical line
inline constexpr u8 space_before = 2;  // whitespace or a comment precedes it on its line
inline constexpr u8 alt = 4;           // spelled as an alternative token ([lex.digraph])
inline constexpr u8 continued = 8;     // a string-literal that phase 5 joined to the one before it
// `export`, `module` or `import` that begins a module or import directive: phase 4's
// export-keyword, module-keyword and import-keyword ([cpp.module], [cpp.import]).
inline constexpr u8 directive = 16;
inline constexpr u8 whitespace = line_start | space_before;
}  // namespace tok_flag

// A token (§8.1): no location. The payload is
// - for `identifier` (and pp-only `header_name`), the index of its spelling in the region's names;
// - for literals and `pp_number`, the index of its entry in the region's literals;
// - for phase-4-only `embed`, the session's interned resource bytes;
// - 0 otherwise.
struct Tok {
  Tk kind = Tk::eof;
  u8 flags = 0;
  u32 payload = 0;
  constexpr bool operator==(const Tok&) const = default;
  constexpr bool is(Tk k) const { return kind == k; }
  constexpr bool has(u8 f) const { return (flags & f) != 0; }
};

// The enumerators of `Tk`, for `template for`.
consteval auto tk_enumerators() { return std::define_static_array(std::meta::enumerators_of(^^Tk)); }

consteval std::string_view tk_spelling_of(std::meta::info e) {
  auto a = std::meta::annotations_of_with_type(e, ^^spelled);
  if (!a.empty()) return std::define_static_string(std::string_view(std::meta::extract<spelled>(a[0]).text));
  std::string_view id = std::meta::identifier_of(e);
  if (id.starts_with("kw_")) return std::define_static_string(id.substr(3));
  return {};
}

inline constexpr std::size_t tk_count = tk_enumerators().size();

// Spelling of each kind: a punctuator's or keyword's text, empty for the others.
inline constexpr std::array<std::string_view, tk_count> tk_spellings = [] {
  std::array<std::string_view, tk_count> s{};
  template for (constexpr auto e : tk_enumerators()) s[std::to_underlying([:e:])] = tk_spelling_of(e);
  return s;
}();

// The enumerator name of each kind, for dumps and tests.
inline constexpr std::array<std::string_view, tk_count> tk_names = [] {
  std::array<std::string_view, tk_count> s{};
  template for (constexpr auto e : tk_enumerators()) {
    s[std::to_underlying([:e:])] = std::define_static_string(std::meta::identifier_of(e));
  }
  return s;
}();

constexpr std::string_view spelling(Tk k) { return tk_spellings[std::to_underlying(k)]; }
constexpr std::string_view name(Tk k) { return tk_names[std::to_underlying(k)]; }

constexpr bool is_keyword(Tk k) { return k >= Tk::kw_alignas && k <= Tk::kw___restrict__; }
constexpr bool is_punctuator(Tk k) { return k >= Tk::l_brace && k <= Tk::hashhash; }
constexpr bool is_literal(Tk k) { return k >= Tk::int_lit && k <= Tk::string_lit; }
constexpr bool is_pp_only(Tk k) {
  return k == Tk::pp_number || k == Tk::header_name || k == Tk::pp_other || k == Tk::embed || k == Tk::hash || k == Tk::hashhash;
}

// The keyword spelled `s`, if any ([lex.key]).
constexpr std::optional<Tk> keyword(std::string_view s) {
  constexpr auto table = [] {
    std::array<std::pair<std::string_view, Tk>, std::to_underlying(Tk::kw___restrict__) - std::to_underlying(Tk::kw_alignas) + 1> t{};
    for (std::size_t i = 0; i < t.size(); ++i) {
      Tk k = Tk(std::to_underlying(Tk::kw_alignas) + i);
      t[i] = {tk_spellings[std::to_underlying(k)], k};
    }
    std::ranges::sort(t);
    return t;
  }();
  auto it = std::ranges::lower_bound(table, s, {}, &std::pair<std::string_view, Tk>::first);
  if (it != table.end() && it->first == s) return it->second;
  return std::nullopt;
}

// The alternative tokens spelled as identifiers ([lex.digraph]) and their primary kinds.
constexpr std::optional<Tk> alternative_token(std::string_view s) {
  constexpr std::array<std::pair<std::string_view, Tk>, 11> table{{
      {"and", Tk::ampamp},
      {"and_eq", Tk::ampequal},
      {"bitand", Tk::amp},
      {"bitor", Tk::pipe},
      {"compl", Tk::tilde},
      {"not", Tk::exclaim},
      {"not_eq", Tk::exclaimequal},
      {"or", Tk::pipepipe},
      {"or_eq", Tk::pipeequal},
      {"xor", Tk::caret},
      {"xor_eq", Tk::caretequal},
  }};
  for (auto [spelling, k] : table) {
    if (spelling == s) return k;
  }
  return std::nullopt;
}

// [lex.charset], [lex.ccon], [lex.string]: the literal encodings, from the encoding-prefix.
enum class Encoding : u8 { ordinary, wide, utf8, utf16, utf32 };

// [lex.icon]: the integer-suffix, without the ud-suffix.
enum class IntSuffix : u8 { none, u, l, ul, ll, ull, z, uz };

// [lex.fcon]: the floating-point-suffix.
enum class FloatSuffix : u8 { none, f, l, f16, f32, f64, f128, bf16 };

}  // namespace ycxx::lex
