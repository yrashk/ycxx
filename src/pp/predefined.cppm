// The predefined macros ([cpp.predefined]), as the text of `<built-in>`. What ycxx defines beyond
// the draft is decided on #112 and recorded in DECISIONS §1.5:
// - its identity, `__ycxx__` and its version, and `__GNUC__` 4.2.1 as Clang has it (#229, which
//   revised #112), not `__clang__`;
// - the target's macros, generated from `ast::Target` (the source of layout), and checked against
//   Clang's for the same target by tests/own/pp/predefined-target.pp.cpp;
// - a feature-test macro only for a feature ycxx implements (`feature_macros`, which a slice that
//   completes a feature updates, as it updates the README's Status).
// `__LINE__`, `__FILE__`, `__DATE__`, `__TIME__` and `__COUNTER__` are dynamic (:unit).
export module ycxx.pp:predefined;

import std;
import ycxx.ast.target;
import ycxx.ast.types;
import :spell;
import :system;

export namespace ycxx::pp {

struct FeatureMacro {
  std::string_view name;
  std::string_view value;
};

// [cpp.predefined], table "Feature-test macros": the features ycxx implements, with the values of
// the pinned draft.
inline constexpr std::array feature_macros{
    FeatureMacro{"__cpp_binary_literals", "201304L"},           // ycxx.lex
    FeatureMacro{"__cpp_decltype_auto", "201304L"},             // θ step 1 (#279)
    FeatureMacro{"__cpp_generic_lambdas", "201304L"},           // θ step 3 (#281): not yet requires-clauses (#350)
    FeatureMacro{"__cpp_hex_float", "201603L"},                 // ycxx.lex
    FeatureMacro{"__cpp_init_captures", "201304L"},             // θ step 2 (#280): not yet packs (#281)
    FeatureMacro{"__cpp_named_character_escapes", "202606L"},   // ycxx.lex
    FeatureMacro{"__cpp_pp_embed", "202606L"},                  // ycxx.pp (#125)
    FeatureMacro{"__cpp_raw_strings", "200710L"},               // ycxx.lex
    FeatureMacro{"__cpp_return_type_deduction", "201304L"},     // θ step 1 (#279)
    FeatureMacro{"__cpp_unicode_literals", "200710L"},          // ycxx.lex
};

}  // namespace ycxx::pp

namespace ycxx::pp {

using ast::Builtin;
using ast::FloatFormat;

// A type as Clang's `__*_TYPE__` macros spell it.
constexpr std::string_view type_name(Builtin b) {
  switch (b) {
  case Builtin::signed_char: return "signed char";
  case Builtin::unsigned_char: return "unsigned char";
  case Builtin::short_: return "short";
  case Builtin::unsigned_short: return "unsigned short";
  case Builtin::int_: return "int";
  case Builtin::unsigned_int: return "unsigned int";
  case Builtin::long_: return "long int";
  case Builtin::unsigned_long: return "long unsigned int";
  case Builtin::long_long: return "long long int";
  case Builtin::unsigned_long_long: return "long long unsigned int";
  default: return "";
  }
}
// The integer-suffix of a constant of that type, and the length modifier of its printf format.
constexpr std::string_view suffix(Builtin b) {
  switch (b) {
  case Builtin::unsigned_int: return "U";
  case Builtin::long_: return "L";
  case Builtin::unsigned_long: return "UL";
  case Builtin::long_long: return "LL";
  case Builtin::unsigned_long_long: return "ULL";
  default: return "";
  }
}
constexpr std::string_view length_modifier(Builtin b) {
  switch (b) {
  case Builtin::signed_char:
  case Builtin::unsigned_char: return "hh";
  case Builtin::short_:
  case Builtin::unsigned_short: return "h";
  case Builtin::long_:
  case Builtin::unsigned_long: return "l";
  case Builtin::long_long:
  case Builtin::unsigned_long_long: return "ll";
  default: return "";
  }
}
constexpr Builtin unsigned_of(Builtin b) {
  switch (b) {
  case Builtin::signed_char: return Builtin::unsigned_char;
  case Builtin::short_: return Builtin::unsigned_short;
  case Builtin::int_: return Builtin::unsigned_int;
  case Builtin::long_: return Builtin::unsigned_long;
  case Builtin::long_long: return Builtin::unsigned_long_long;
  default: return b;
  }
}

// A floating constant with `digits` significant digits (0: its exact decimal value), as Clang
// prints `__FLT_MAX__`: no leading zeros in the exponent.
std::string floating(double v, int digits, std::string_view suffix) {
  std::string s = std::format("{:.{}e}", v, digits == 0 ? 40 : digits - 1);
  if (digits == 0) {  // the exact value: trailing zeros of the mantissa dropped
    auto e = s.find('e');
    auto last = s.find_last_not_of('0', e - 1);
    s.erase(s[last] == '.' ? last : last + 1, e - (s[last] == '.' ? last : last + 1));
  }
  auto e = s.find('e');
  std::string mantissa = s.substr(0, e);
  char sign = s[e + 1];
  std::string exponent = s.substr(e + 2);
  exponent.erase(0, std::min(exponent.find_first_not_of('0'), exponent.size() - 1));
  return std::format("{}e{}{}{}", mantissa, sign == '-' ? "-" : "+", exponent, suffix);
}

// `exact`: the limits printed exactly (Clang does so for _Float16's).
void float_family(std::string& out, std::string_view prefix, FloatFormat f, std::string_view suf, bool exact = false) {
  auto shape = ast::shape_of(f);
  const int p = shape.precision;
  const int max_exp = 1 << (shape.exponent - 1);
  const int min_exp = 3 - max_exp;
  const double log2 = std::log10(2.0);
  const int dig = static_cast<int>(std::floor((p - 1) * log2));
  const int decimal_dig = static_cast<int>(std::ceil(1 + p * log2));
  const double max_significand = 2.0 - std::ldexp(1.0, 1 - p);
  const int digits = exact ? 0 : decimal_dig;
  // The x87 limits rounded to 21 significant digits. Host long double can be binary64,
  // so generate those spellings from the target format without host floating-point conversion.
  auto limit = [&](double significand, int exponent, std::string_view x87) {
    if (f == FloatFormat::x87_extended) return std::format("{}{}", x87, suf);
    return floating(std::ldexp(significand, exponent), digits, suf);
  };
  auto max = limit(max_significand, max_exp - 1, "1.18973149535723176502e+4932");
  auto def = [&](std::string_view name, std::string value) { out += std::format("#define __{}_{}__ {}\n", prefix, name, value); };
  def("DECIMAL_DIG", std::to_string(decimal_dig));
  def("DENORM_MIN", limit(1.0, min_exp - p, "3.64519953188247460253e-4951"));
  def("DIG", std::to_string(dig));
  def("EPSILON", limit(1.0, 1 - p, "1.08420217248550443401e-19"));
  def("HAS_DENORM", "1");
  def("HAS_INFINITY", "1");
  def("HAS_QUIET_NAN", "1");
  def("MANT_DIG", std::to_string(p));
  def("MAX_10_EXP", std::to_string(static_cast<int>(std::floor((max_exp - 1) * log2 + std::log10(max_significand)))));
  def("MAX_EXP", std::to_string(max_exp));
  def("MAX", max);
  def("MIN_10_EXP", std::format("({})", static_cast<int>(std::ceil((min_exp - 1) * log2))));
  def("MIN_EXP", std::format("({})", min_exp));
  def("MIN", limit(1.0, min_exp - 1, "3.36210314311209350626e-4932"));
  def("NORM_MAX", max);
}

std::string target_macros(const ast::Target& t) {
  std::string out;
  auto def = [&](std::string_view name, auto value) { out += std::format("#define {} {}\n", name, value); };
  auto bits = [&](Builtin b) { return 8u * t[b].size; };
  auto max_of = [&](Builtin b) -> std::string {
    unsigned n = bits(b);
    bool uns = ast::is_standard_unsigned(b);
    std::uint64_t m = n == 64 ? (uns ? ~0ull : ~0ull >> 1) : (uns ? (1ull << n) - 1 : (1ull << (n - 1)) - 1);
    return std::format("{}{}", m, suffix(b));
  };
  const bool darwin = t.triple == ast::aarch64_darwin.triple;
  const bool linux_target = t.triple == ast::x86_64_linux.triple;
  const bool aarch64 = t.triple.starts_with("arm64") || t.triple.starts_with("aarch64");
  if (darwin) {
    def("__APPLE__", 1);
    def("__MACH__", 1);
    for (std::string_view os : {"DRIVERKIT", "EMBEDDED", "FIRMWARE", "IOS", "IPHONE", "LINUX", "MACCATALYST", "NANO",
                                "SIMULATOR", "TV", "UEFI", "UIKITFORMAC", "UNIX", "VISION", "WATCH", "WIN32", "WINDOWS"})
      def(std::format("TARGET_OS_{}", os), 0);
    def("TARGET_OS_MAC", 1);
    def("TARGET_OS_OSX", 1);
    def("__USER_LABEL_PREFIX__", "_");
  }
  if (linux_target) {
    for (std::string_view macro : {"__ELF__", "__gnu_linux__", "__linux", "__linux__", "__unix", "__unix__"})
      def(macro, 1);
    def("__USER_LABEL_PREFIX__", "");
  }
  if (aarch64) {
    def("__aarch64__", 1);
    if (darwin) {
      def("__arm64", 1);
      def("__arm64__", 1);
    }
  }
  if (t.triple.starts_with("x86_64")) {
    for (std::string_view macro : {"__amd64", "__amd64__", "__x86_64", "__x86_64__"}) def(macro, 1);
  }
  if (t.pointer_size == 8 && t[Builtin::long_].size == 8) {
    def("_LP64", 1);
    def("__LP64__", 1);
  }
  def("__ORDER_LITTLE_ENDIAN__", 1234);
  def("__ORDER_BIG_ENDIAN__", 4321);
  def("__ORDER_PDP_ENDIAN__", 3412);
  def("__BYTE_ORDER__", "__ORDER_LITTLE_ENDIAN__");
  def("__LITTLE_ENDIAN__", 1);
  def("__CHAR_BIT__", 8);
  def("__BOOL_WIDTH__", 1);
  def("__POINTER_WIDTH__", 8 * t.pointer_size);
  def("__BIGGEST_ALIGNMENT__", t[Builtin::long_double].align > t[Builtin::long_long].align ? t[Builtin::long_double].align
                                                                                            : t[Builtin::long_long].align);
  if (!t.char_signed) def("__CHAR_UNSIGNED__", 1);
  // Sizes.
  def("__SIZEOF_SHORT__", t[Builtin::short_].size);
  def("__SIZEOF_INT__", t[Builtin::int_].size);
  def("__SIZEOF_LONG__", t[Builtin::long_].size);
  def("__SIZEOF_LONG_LONG__", t[Builtin::long_long].size);
  def("__SIZEOF_POINTER__", t.pointer_size);
  def("__SIZEOF_FLOAT__", t[Builtin::float_].size);
  def("__SIZEOF_DOUBLE__", t[Builtin::double_].size);
  def("__SIZEOF_LONG_DOUBLE__", t[Builtin::long_double].size);
  def("__SIZEOF_SIZE_T__", t[t.size_type].size);
  def("__SIZEOF_PTRDIFF_T__", t[t.ptrdiff_type].size);
  def("__SIZEOF_WCHAR_T__", t[Builtin::wchar_t_].size);
  def("__SIZEOF_WINT_T__", t[t.wint_type].size);
  // Limits and widths of the standard integer types.
  def("__SCHAR_MAX__", max_of(Builtin::signed_char));
  def("__SHRT_MAX__", max_of(Builtin::short_));
  def("__SHRT_WIDTH__", bits(Builtin::short_));
  def("__INT_MAX__", max_of(Builtin::int_));
  def("__INT_WIDTH__", bits(Builtin::int_));
  def("__LONG_MAX__", max_of(Builtin::long_));
  def("__LONG_WIDTH__", bits(Builtin::long_));
  def("__LONG_LONG_MAX__", max_of(Builtin::long_long));
  def("__LLONG_WIDTH__", bits(Builtin::long_long));
  // The typedefs' types, limits, widths and formats.
  auto typedef_family = [&](std::string_view name, Builtin b, bool width, bool min) {
    def(std::format("__{}_TYPE__", name), type_name(b));
    def(std::format("__{}_MAX__", name), max_of(b));
    if (width) def(std::format("__{}_WIDTH__", name), bits(b));
    if (min) {
      auto value = ast::is_standard_unsigned(b) ? std::format("0{}", suffix(b)) : std::format("(-__{}_MAX__ - 1)", name);
      def(std::format("__{}_MIN__", name), value);
    }
  };
  typedef_family("WCHAR", t.wchar_underlying, true, true);
  typedef_family("WINT", t.wint_type, true, true);
  if (ast::is_standard_unsigned(t.wint_type)) def("__WINT_UNSIGNED__", 1);
  typedef_family("SIG_ATOMIC", t.sig_atomic_type, true, true);
  typedef_family("SIZE", t.size_type, true, false);
  typedef_family("PTRDIFF", t.ptrdiff_type, true, false);
  typedef_family("INTMAX", t.intmax_type, true, false);
  typedef_family("UINTMAX", unsigned_of(t.intmax_type), true, false);
  typedef_family("INTPTR", t.intptr_type, true, false);
  typedef_family("UINTPTR", unsigned_of(t.intptr_type), true, false);
  def("__CHAR16_TYPE__", type_name(t.char16_underlying));
  def("__CHAR32_TYPE__", type_name(t.char32_underlying));
  auto formats = [&](std::string_view name, Builtin b) {
    std::string_view m = length_modifier(b);
    if (ast::is_standard_unsigned(b)) {
      for (char c : {'o', 'u', 'x', 'X'}) out += std::format("#define __{}_FMT{}__ \"{}{}\"\n", name, c, m, c);
    } else {
      for (char c : {'d', 'i'}) out += std::format("#define __{}_FMT{}__ \"{}{}\"\n", name, c, m, c);
    }
  };
  formats("SIZE", t.size_type);
  formats("PTRDIFF", t.ptrdiff_type);
  formats("INTMAX", t.intmax_type);
  formats("UINTMAX", unsigned_of(t.intmax_type));
  formats("INTPTR", t.intptr_type);
  formats("UINTPTR", unsigned_of(t.intptr_type));
  for (Builtin b : t.exact_width_int_types) {
    auto n = bits(b);
    for (Builtin x : {b, unsigned_of(b)}) {
      std::string_view u = ast::is_standard_unsigned(x) ? "U" : "";
      for (std::string_view kind : {"", "_LEAST", "_FAST"}) {
        std::string name = std::format("{}INT{}{}", u, kind, n);
        def(std::format("__{}_TYPE__", name), type_name(x));
        def(std::format("__{}_MAX__", name), max_of(x));
        if (!kind.empty()) def(std::format("__{}_WIDTH__", name), bits(x));
        formats(name, x);
      }
      std::string c = std::format("{}INT{}", u, n);
      def(std::format("__{}_C_SUFFIX__", c), suffix(x));
      std::string_view s = suffix(x);
      // `c ## L` as GCC and glibc spell it: glibc's <stdint.h> redefines __INT64_C and
      // __UINT64_C, and a redefinition must match in white-space separation too (#657).
      if (s.empty()) out += std::format("#define __{}_C(c) c\n", c);
      else out += std::format("#define __{}_C(c) c ## {}\n", c, s);
    }
  }
  for (auto [name, b] : {std::pair{"INTMAX", t.intmax_type}, {"UINTMAX", unsigned_of(t.intmax_type)}}) {
    def(std::format("__{}_C_SUFFIX__", name), suffix(b));
    out += std::format("#define __{}_C(c) c ## {}\n", name, suffix(b));
  }
  // The standard floating types and the _Float16 extension.
  def("__FLT_RADIX__", 2);
  // FLT_EVAL_METHOD ([cfloat.syn], C 5.2.4.2.2): every target evaluates in the operands' own type.
  def("__FLT_EVAL_METHOD__", 0);
  if (ast::shape_of(t[Builtin::float_].format).precision <= 53) float_family(out, "FLT", t[Builtin::float_].format, "F");
  if (ast::shape_of(t[Builtin::double_].format).precision <= 53) float_family(out, "DBL", t[Builtin::double_].format, "");
  float_family(out, "LDBL", t[Builtin::long_double].format, "L");
  def("__DECIMAL_DIG__", "__LDBL_DECIMAL_DIG__");
  if (t.has(Builtin::float16)) float_family(out, "FLT16", t[Builtin::float16].format, "F16", true);
  return out;
}

}  // namespace ycxx::pp

export namespace ycxx::pp {

// The text of `<built-in>` for a target: identity, the draft's predefined macros, the
// implemented features, and the target's.
std::string predefined_text(const ast::Target& t) {
  std::string s;
  auto def = [&](std::string_view name, auto value) { s += std::format("#define {} {}\n", name, value); };
  def("__ycxx__", 1);
  def("__ycxx_major__", version_major);
  def("__ycxx_minor__", version_minor);
  def("__ycxx_patchlevel__", version_patch);
  // The GNU dialect, at the version Clang claims (#229): the SDK's headers test `__GNUC__` for a
  // supported compiler, and the GNU extensions their `__GNUC__` paths use are in
  // docs/EXTENSIONS.md. libycxx tells ycxx from GCC by `__ycxx__`, which it tests first.
  def("__GNUC__", 4);
  def("__GNUC_MINOR__", 2);
  def("__GNUC_PATCHLEVEL__", 1);
  // [cpp.predefined]/1.
  def("__cplusplus", "202400L");
  def("__STDC_EMBED_NOT_FOUND__", 0);
  def("__STDC_EMBED_FOUND__", 1);
  def("__STDC_EMBED_EMPTY__", 2);
  def("__STDC_HOSTED__", 1);
  def("__STDCPP_DEFAULT_NEW_ALIGNMENT__", std::format("{}UL", 2 * t.pointer_size));
  def("__STDCPP_THREADS__", 1);
  // [cpp.predefined]/2: the extended floating-point types the target has, once ycxx supports
  // floating-point values (#201); until then a header must not take them for available
  // (DECISIONS §1.5).
  constexpr bool floating_values = false;
  if (floating_values && t.has(Builtin::float16)) def("__STDCPP_FLOAT16_T__", 1);
  if (floating_values && t.has(Builtin::float32)) def("__STDCPP_FLOAT32_T__", 1);
  if (floating_values && t.has(Builtin::float64)) def("__STDCPP_FLOAT64_T__", 1);
  if (floating_values && t.has(Builtin::float128)) def("__STDCPP_FLOAT128_T__", 1);
  if (floating_values && t.has(Builtin::bfloat16)) def("__STDCPP_BFLOAT16_T__", 1);
  for (const FeatureMacro& m : feature_macros) def(m.name, m.value);
  return s + target_macros(t);
}

}  // namespace ycxx::pp
