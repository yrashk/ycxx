// Literal values: integer and floating literals from pp-numbers ([lex.icon], [lex.fcon]),
// character literals ([lex.ccon]), and the concatenation and encoding of adjacent string literals
// in phase 5 ([lex.string]). `ud_suffix` is set for user-defined literals ([lex.ext]).
//
// Implementation-defined choices ([lex.charset], [lex.ccon], [lex.fcon]):
// - the ordinary literal encoding is UTF-8 and the wide literal encoding is UTF-32;
// - a multicharacter literal's value is its code units, first most significant, in 32 bits;
// - conditional-escape-sequences are not supported (ill-formed);
// - an inexact floating value is rounded to nearest, ties to even, and a nonzero value smaller
//   than the type's least subnormal becomes zero; a value beyond the type's finite range is an
//   infinity, which [lex.fcon]/4 allows (its example), with a warning, as Clang and GCC give.
export module ycxx.lex:literal;

import std;
import :tok;
import :diag;
import :unicode;
import :lexer;
import :extended_float;

export namespace ycxx::lex {

// Where a literal's diagnostic goes: a byte offset within the literal's spelling, the kind and
// two arguments.
template <class F>
concept Reporter = std::invocable<F&, u32, LexDiag, u64, u64>;

// Phase 6 for a pp-number: `int_lit` or `float_lit`, the literal it most resembles when it is
// ill-formed (reported, and `bad` set).
template <Reporter R>
Tk classify_number(Literal& l, Tables& names, R&& report);

// [lex.ccon]: fills a character literal's value.
template <Reporter R>
void evaluate_char(Literal& l, R&& report);

// Phase 5 ([lex.string], [lex.ext]): concatenates a run of adjacent string literals into the
// first one's `units`, with the run's common encoding and ud-suffix. `report` takes the index of
// the literal in the run as well.
template <class R>
void concatenate_strings(std::span<Literal*> run, R&& report);

// The shared lookup spelling of a literal-operator-id ([over.literal]).
inline constexpr std::string_view literal_operator_prefix = "operator\"\"";

std::string literal_operator_name(std::string_view suffix) {
  std::string name(literal_operator_prefix);
  name += suffix;
  return name;
}

// The code units a sequence of characters encodes to.
void encode(Encoding e, char32_t c, std::vector<u32>& out);

}  // namespace ycxx::lex

namespace ycxx::lex {

constexpr bool is_dec(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_oct(char c) { return c >= '0' && c <= '7'; }

// The largest value of a code unit of each encoding ([basic.fundamental]: char and char8_t have 8
// bits, char16_t 16, char32_t and wchar_t 32 on every supported target).
constexpr u64 unit_max(Encoding e) {
  switch (e) {
  case Encoding::ordinary:
  case Encoding::utf8: return 0xFF;
  case Encoding::utf16: return 0xFFFF;
  case Encoding::utf32:
  case Encoding::wide: return 0xFFFFFFFF;
  }
  return 0;
}

// The ud-suffix spelled at the start of `s`, with its UCNs replaced, if `s` is exactly one
// identifier ([lex.ext]).
std::optional<std::string> suffix_identifier(std::string_view s) {
  std::string out;
  std::size_t i = 0;
  for (bool start = true; i < s.size(); start = false) {
    char32_t c = 0;
    std::size_t length = 0;
    if (s[i] == '\\') {
      auto u = parse_ucn(s, i);
      if (!u) return std::nullopt;
      c = u->error ? 0xFFFD : u->value;
      length = u->length;
      if (!u->error && c >= 0xA0 && !(start ? unicode::is_identifier_start(c) : unicode::is_identifier_continue(c))) {
        return std::nullopt;
      }
    } else if (auto d = unicode::decode_utf8(s.substr(i))) {
      c = d->c;
      length = d->length;
      if (!(start ? unicode::is_identifier_start(c) : unicode::is_identifier_continue(c))) return std::nullopt;
    } else {
      return std::nullopt;
    }
    unicode::append_utf8(out, c);
    i += length;
  }
  if (out.empty()) return std::nullopt;
  return out;
}

// A ud-suffix's name; a ud-suffix is an identifier, so it must be in NFC ([lex.name]/1).
template <class R>
u32 suffix_name(const std::string& id, u32 at, Tables& names, R& report) {
  std::u32string cps;
  for (std::size_t i = 0; i < id.size();) {
    auto d = unicode::decode_utf8(std::string_view(id).substr(i));
    if (!d) break;
    cps += d->c;
    i += d->length;
  }
  if (!unicode::is_nfc(cps)) report(at, LexDiag::identifier_not_nfc, 0, 0);
  return names.name(id);
}

// A digit sequence with optional single-quote separators, each between two digits of `base`.
// Appends the digits to `digits` and returns the length consumed.
std::size_t digit_sequence(std::string_view s, std::size_t i, bool (*ok)(char), std::string& digits) {
  std::size_t j = i;
  while (j < s.size()) {
    if (ok(s[j])) {
      digits += s[j++];
    } else if (s[j] == '\'' && j > i && j + 1 < s.size() && ok(s[j + 1])) {
      ++j;
    } else {
      break;
    }
  }
  return j - i;
}

// The standard suffixes, exactly.
std::optional<IntSuffix> int_suffix(std::string_view s) {
  std::string t(s);
  for (char& c : t) c = static_cast<char>(c | 0x20);
  // `lL` and `Ll` are not long-long-suffixes.
  if ((s.find("lL") != s.npos) || (s.find("Ll") != s.npos)) return std::nullopt;
  if (t.empty()) return IntSuffix::none;
  if (t == "u") return IntSuffix::u;
  if (t == "l") return IntSuffix::l;
  if (t == "ul" || t == "lu") return IntSuffix::ul;
  if (t == "ll") return IntSuffix::ll;
  if (t == "ull" || t == "llu") return IntSuffix::ull;
  if (t == "z") return IntSuffix::z;
  if (t == "uz" || t == "zu") return IntSuffix::uz;
  return std::nullopt;
}

std::optional<FloatSuffix> float_suffix(std::string_view s) {
  constexpr std::array<std::pair<std::string_view, FloatSuffix>, 15> table{{
      {"", FloatSuffix::none},     {"f", FloatSuffix::f},       {"F", FloatSuffix::f},
      {"l", FloatSuffix::l},       {"L", FloatSuffix::l},       {"f16", FloatSuffix::f16},
      {"F16", FloatSuffix::f16},   {"f32", FloatSuffix::f32},   {"F32", FloatSuffix::f32},
      {"f64", FloatSuffix::f64},   {"F64", FloatSuffix::f64},   {"f128", FloatSuffix::f128},
      {"F128", FloatSuffix::f128}, {"bf16", FloatSuffix::bf16}, {"BF16", FloatSuffix::bf16},
  }};
  for (auto [t, k] : table) {
    if (t == s) return k;
  }
  return std::nullopt;
}

template <class T>
std::optional<u64> float_bits(std::string_view text, std::chars_format fmt, bool at_least_one, bool& overflow) {
  T v{};
  auto r = std::from_chars(text.data(), text.data() + text.size(), v, fmt);
  if (r.ec == std::errc::result_out_of_range) {
    if (at_least_one) {
      overflow = true;
      return std::nullopt;
    }
    v = T(0);  // nonzero but below the least subnormal
  } else if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  if constexpr (sizeof(T) == 4) return std::bit_cast<std::uint32_t>(v);
  else return std::bit_cast<std::uint64_t>(v);
}

// One element of a character or string literal's contents.
struct Element {
  enum Kind : u8 { character, numeric, bad } kind = bad;
  u64 value = 0;
  std::size_t begin = 0;
};

// Reads the c-char or s-char at `s[i]` ([lex.ccon]). Reports ill-formed escapes.
template <Reporter R>
Element next_element(std::string_view s, std::size_t& i, u32 base, R& report) {
  Element e{Element::bad, 0, i};
  if (s[i] != '\\') {
    if (auto d = unicode::decode_utf8(s.substr(i))) {
      e = {Element::character, d->c, i};
      i += d->length;
    } else {
      ++i;  // an ill-formed UTF-8 byte, diagnosed in phase 1
    }
    return e;
  }
  char k = i + 1 < s.size() ? s[i + 1] : '\0';
  auto simple = [&](char32_t c) {
    i += 2;
    return Element{Element::character, c, e.begin};
  };
  switch (k) {
  case '\'': return simple('\'');
  case '"': return simple('"');
  case '?': return simple('?');
  case '\\': return simple('\\');
  case 'a': return simple(0x07);
  case 'b': return simple(0x08);
  case 'f': return simple(0x0C);
  case 'n': return simple(0x0A);
  case 'r': return simple(0x0D);
  case 't': return simple(0x09);
  case 'v': return simple(0x0B);
  default: break;
  }
  if (k == 'u' || k == 'U' || k == 'N') {
    if (auto u = parse_ucn(s, i)) {
      i += u->length;
      if (u->error) {
        report(base + u32(e.begin), *u->error, u->value, 0);
        return e;
      }
      return {Element::character, u->value, e.begin};
    }
    report(base + u32(e.begin), LexDiag::ucn_incomplete, 0, 0);
    i += 2;
    return e;
  }
  if (is_oct(k) || k == 'o' || k == 'x') {
    bool delimited = (k == 'o' || k == 'x') && i + 2 < s.size() && s[i + 2] == '{';
    bool hex = k == 'x';
    std::size_t j = k == 'o' || k == 'x' ? i + 2 + (delimited ? 1 : 0) : i + 1;
    std::size_t first = j;
    u64 v = 0;
    bool big = false;
    while (j < s.size() && (hex ? is_hex(s[j]) : is_oct(s[j])) && (delimited || hex || j - first < 3)) {
      u64 d = hex ? hex_value(s[j]) : u64(s[j] - '0');
      if (v > (~u64(0) - d) / (hex ? 16 : 8)) big = true;
      v = v * (hex ? 16 : 8) + d;
      ++j;
    }
    if (k == 'o' && !delimited) {
      // `\o` without a brace is a conditional-escape-sequence.
      report(base + u32(e.begin), LexDiag::unknown_escape, u64('o'), 0);
      i += 2;
      return e;
    }
    if (j == first) {
      // `\o{9}`: digits, but not of the base, end the sequence before its `}`.
      bool other = delimited && j < s.size() && s[j] != '}';
      report(base + u32(e.begin), other ? LexDiag::unterminated_escape : LexDiag::empty_escape, 0, 0);
      i = delimited && j < s.size() && s[j] == '}' ? j + 1 : j;
      return e;
    }
    if (delimited) {
      if (j >= s.size() || s[j] != '}') {
        report(base + u32(e.begin), LexDiag::unterminated_escape, 0, 0);
        i = j;
        return e;
      }
      ++j;
    }
    i = j;
    return {Element::numeric, big ? ~u64(0) : v, e.begin};
  }
  // A conditional-escape-sequence, or a backslash before a non-basic character.
  std::size_t at = i + 1;
  char32_t c = 0;
  if (auto d = unicode::decode_utf8(s.substr(at))) {
    c = d->c;
    i = at + d->length;
  } else {
    i = at + 1;
  }
  report(base + u32(e.begin), LexDiag::unknown_escape, c, 0);
  return e;
}

// The contents of a non-raw literal: between the quotes, after the prefix.
std::string_view quoted_body(const Literal& l, std::size_t& body_begin) {
  char q = l.spelling.find('\'') < l.spelling.find('"') ? '\'' : '"';
  std::size_t open = l.spelling.find(q);
  std::size_t close = l.spelling.rfind(q);
  body_begin = open + 1;
  return std::string_view(l.spelling).substr(open + 1, close - open - 1);
}

// The contents of a raw string literal: between `(delimiter` and `)delimiter"`.
std::string_view raw_body(const Literal& l, std::size_t& body_begin) {
  std::string_view s = l.spelling;
  std::size_t quote = s.find('"');
  std::size_t paren = s.find('(', quote);
  std::string closing = ")";
  closing += s.substr(quote + 1, paren - quote - 1);
  closing += '"';
  std::size_t close = s.rfind(closing);
  body_begin = paren + 1;
  return s.substr(paren + 1, close - paren - 1);
}

}  // namespace ycxx::lex

export namespace ycxx::lex {

void encode(Encoding e, char32_t c, std::vector<u32>& out) {
  switch (e) {
  case Encoding::ordinary:
  case Encoding::utf8: {
    std::string bytes;
    unicode::append_utf8(bytes, c);
    for (char b : bytes) out.push_back(static_cast<unsigned char>(b));
    break;
  }
  case Encoding::utf16:
    if (c >= 0x10000) {
      out.push_back(0xD800 + ((c - 0x10000) >> 10));
      out.push_back(0xDC00 + ((c - 0x10000) & 0x3FF));
    } else {
      out.push_back(c);
    }
    break;
  case Encoding::utf32:
  case Encoding::wide: out.push_back(c); break;
  }
}

template <Reporter R>
Tk classify_number(Literal& l, Tables& names, R&& report) {
  std::string_view s = l.spelling;
  std::string digits;
  std::size_t i = 0;
  bool is_float = false;
  bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
  bool bin = s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B');
  std::string fraction;
  std::string exponent;  // with its sign
  auto exponent_part = [&](char lower) {
    if (i < s.size() && (s[i] | 0x20) == lower) {
      std::size_t j = i + 1;
      std::string e;
      if (j < s.size() && (s[j] == '+' || s[j] == '-')) e += s[j++];
      std::size_t n = digit_sequence(s, j, is_dec, e);
      if (n > 0) {
        exponent = e;
        i = j + n;
        return true;
      }
    }
    return false;
  };
  if (hex) {
    i = 2;
    i += digit_sequence(s, i, is_hex, digits);
    std::size_t mantissa_end = i;
    if (i < s.size() && s[i] == '.') {
      std::size_t j = i + 1;
      std::size_t n = digit_sequence(s, j, is_hex, fraction);
      if (!digits.empty() || n > 0) {
        i = j + n;
        if (exponent_part('p')) is_float = true;
        else i = mantissa_end, fraction.clear();  // a hexadecimal fraction needs an exponent
      }
    } else if (!digits.empty() && exponent_part('p')) {
      is_float = true;
    }
    if (digits.empty() && !is_float) {
      // `0x` alone: the octal literal `0` with ud-suffix `x…`.
      hex = false;
      i = 1;
      digits = "0";
    }
    l.base = hex ? 16 : 8;
  } else if (bin) {
    i = 2 + digit_sequence(s, 2, [](char c) { return c == '0' || c == '1'; }, digits);
    if (digits.empty()) {
      i = 1;
      digits = "0";
      l.base = 8;
    } else {
      l.base = 2;
    }
  } else {
    i = digit_sequence(s, 0, is_dec, digits);
    if (i < s.size() && s[i] == '.') {
      std::size_t j = i + 1;
      std::size_t n = digit_sequence(s, j, is_dec, fraction);
      if (!digits.empty() || n > 0) {
        is_float = true;
        i = j + n;
        exponent_part('e');
      }
    } else if (!digits.empty() && exponent_part('e')) {
      is_float = true;
    }
    l.base = 10;
    if (!is_float && digits.starts_with('0')) {
      l.base = 8;
      // The octal literal ends at its first non-octal digit.
      std::size_t n = 1;
      std::size_t k = 1;
      while (k < s.size() && n < digits.size()) {
        if (s[k] == '\'') {
          ++k;
          continue;
        }
        if (!is_oct(s[k])) break;
        ++k;
        ++n;
      }
      digits.resize(n);
      i = k;
    }
  }
  std::string_view rest = s.substr(i);
  if (is_float) {
    l.base = hex ? 16 : 10;
    if (auto fs = float_suffix(rest)) {
      l.float_suffix = *fs;
    } else if (auto id = suffix_identifier(rest); id && !rest.empty()) {
      l.ud_suffix = suffix_name(*id, u32(i), names, report);
    } else {
      report(u32(i), LexDiag::invalid_number, u64(static_cast<unsigned char>(s[i])), 0);
      l.bad = true;
      return Tk::float_lit;
    }
    if (l.ud_suffix == no_name && (l.float_suffix == FloatSuffix::none || l.float_suffix == FloatSuffix::f ||
                                   l.float_suffix == FloatSuffix::l)) {
      std::string text = digits;
      if (!fraction.empty() || hex) text += '.' + fraction;
      text += (hex ? "p" : "e") + (exponent.empty() ? std::string("0") : exponent);
      // Out of range is overflow if the value is at least one, else it underflows to zero: the
      // position of the first nonzero digit, scaled, plus the exponent.
      long long magnitude = 0;
      if (auto nz = digits.find_first_not_of('0'); nz != digits.npos) {
        l.nonzero_float_spelling = true;
        magnitude = static_cast<long long>(digits.size() - nz) * (hex ? 4 : 1);
      } else if (auto fz = fraction.find_first_not_of('0'); fz != fraction.npos) {
        l.nonzero_float_spelling = true;
        magnitude = -static_cast<long long>(fz) * (hex ? 4 : 1);
      }
      long long e = 0;
      std::from_chars(exponent.data() + (exponent.starts_with('+') ? 1 : 0), exponent.data() + exponent.size(), e);
      if (exponent.size() > 12) e = exponent.starts_with('-') ? -(1LL << 40) : (1LL << 40);
      bool at_least_one = magnitude + e > 0;
      bool overflow = false;
      auto fmt = hex ? std::chars_format::hex : std::chars_format::scientific;
      auto bits = l.float_suffix == FloatSuffix::f ? float_bits<float>(text, fmt, at_least_one, overflow)
                                                    : float_bits<double>(text, fmt, at_least_one, overflow);
      if (l.float_suffix == FloatSuffix::l) {
        l.x87 = x87_literal(digits, fraction, exponent, hex);
        l.x87_overflow = l.x87[1] == 0x7fff;
        l.overflow = overflow;
      }
      if (overflow) {
        if (l.float_suffix != FloatSuffix::l) report(0, LexDiag::float_out_of_range, 0, 0);
        bits = l.float_suffix == FloatSuffix::f ? u64(std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity()))
                                                : std::bit_cast<u64>(std::numeric_limits<double>::infinity());
      }
      l.value = bits.value_or(0);
    }
    return Tk::float_lit;
  }
  if (auto is = int_suffix(rest)) {
    l.int_suffix = *is;
  } else if (auto id = suffix_identifier(rest); id && !rest.empty()) {
    l.ud_suffix = suffix_name(*id, u32(i), names, report);
  } else {
    report(u32(i), LexDiag::invalid_number, u64(static_cast<unsigned char>(s[i])), 0);
    l.bad = true;
    return Tk::int_lit;
  }
  u64 v = 0;
  u64 radix = l.base;
  for (char c : digits) {
    u64 d = hex_value(c);
    if (v > (~u64(0) - d) / radix) l.overflow = true;
    v = v * radix + d;
  }
  l.value = l.overflow ? 0 : v;
  if (l.overflow && l.ud_suffix == no_name) {
    report(0, LexDiag::integer_too_large, 0, 0);
    l.bad = true;
    return Tk::int_lit;
  }
  return Tk::int_lit;
}

template <Reporter R>
void evaluate_char(Literal& l, R&& report) {
  if (l.bad) return;
  std::size_t body_begin = 0;
  std::string_view body = quoted_body(l, body_begin);
  std::vector<Element> elements;
  for (std::size_t i = 0; i < body.size();) elements.push_back(next_element(body, i, u32(body_begin), report));
  if (std::ranges::any_of(elements, [](const Element& e) { return e.kind == Element::bad; })) {
    l.bad = true;
    return;
  }
  auto at = [&](const Element& e) { return u32(body_begin + e.begin); };
  if (elements.size() == 1) {
    const Element& e = elements[0];
    if (e.kind == Element::numeric) {
      if (e.value > unit_max(l.encoding)) {
        report(at(e), LexDiag::escape_out_of_range, e.value, 0);
        l.bad = true;
        return;
      }
      l.value = e.value;
      return;
    }
    std::vector<u32> units;
    encode(l.encoding, char32_t(e.value), units);
    if (units.size() != 1) {
      report(at(e), LexDiag::char_not_single_unit, e.value, 0);
      l.bad = true;
      return;
    }
    l.value = units[0];
    return;
  }
  l.multichar = true;
  if (l.encoding != Encoding::ordinary) {
    report(0, LexDiag::multichar_with_prefix, 0, 0);
    l.bad = true;
    return;
  }
  std::uint32_t v = 0;
  for (const Element& e : elements) {
    u64 unit = e.value;
    if (e.kind == Element::numeric ? unit > 0xFF : unit >= 0x80) {
      report(at(e), LexDiag::multichar_not_single_unit, unit, 0);
      l.bad = true;
      return;
    }
    v = (v << 8) | std::uint32_t(unit);
  }
  report(0, LexDiag::multichar, 0, 0);
  l.value = v;
}

template <class R>
void concatenate_strings(std::span<Literal*> run, R&& report) {
  Encoding common = Encoding::ordinary;
  u32 suffix = no_name;
  bool bad = false;
  for (std::size_t k = 0; k < run.size(); ++k) {
    const Literal& l = *run[k];
    if (l.encoding != Encoding::ordinary) {
      if (common != Encoding::ordinary && common != l.encoding) {
        report(k, u32(0), LexDiag::concat_encoding_mismatch, 0, 0);
        bad = true;
      }
      common = l.encoding;
    }
    if (l.ud_suffix != no_name) {
      if (suffix != no_name && suffix != l.ud_suffix) {
        report(k, u32(0), LexDiag::concat_suffix_mismatch, 0, 0);
        bad = true;
      }
      suffix = l.ud_suffix;
    }
    bad = bad || l.bad;
  }
  Literal& head = *run[0];
  std::vector<u32> units;
  for (std::size_t k = 0; k < run.size() && !bad; ++k) {
    const Literal& l = *run[k];
    auto at_k = [&](u32 offset, LexDiag d, u64 a, u64 b) { report(k, offset, d, a, b); };
    std::size_t body_begin = 0;
    if (l.raw) {
      std::string_view body = raw_body(l, body_begin);
      for (std::size_t i = 0; i < body.size();) {
        if (auto d = unicode::decode_utf8(body.substr(i))) {
          encode(common, d->c, units);
          i += d->length;
        } else {
          units.push_back(static_cast<unsigned char>(body[i++]));
        }
      }
      continue;
    }
    std::string_view body = quoted_body(l, body_begin);
    for (std::size_t i = 0; i < body.size();) {
      Element e = next_element(body, i, u32(body_begin), at_k);
      if (e.kind == Element::bad) {
        bad = true;
      } else if (e.kind == Element::numeric) {
        if (e.value > unit_max(common)) {
          at_k(u32(body_begin + e.begin), LexDiag::escape_out_of_range, e.value, 0);
          bad = true;
        }
        units.push_back(u32(e.value));
      } else {
        encode(common, char32_t(e.value), units);
      }
    }
  }
  head.encoding = common;
  head.ud_suffix = suffix;
  head.bad = bad;
  head.units = bad ? std::vector<u32>{} : std::move(units);
}

}  // namespace ycxx::lex
