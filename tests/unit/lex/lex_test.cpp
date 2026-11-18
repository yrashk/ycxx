// The lexer and the directive-free `pp` (#40), from [lex]: phases 1–2, comments, every kind of
// preprocessing token and token, literal values, phase 5, directives as unsupported, locations,
// and `pp` as an engine query.
import std;
import ycxx.engine;
import ycxx.pp;
import ycxx.unit;

using namespace ycxx;
using lex::LexDiag;
using lex::Tk;
using lex::Tok;
using pp::Region;
using ycxx::unit::check;
namespace tok_flag = lex::tok_flag;

namespace {

std::vector<Tk> kinds(const Region& r) {
  std::vector<Tk> out;
  for (Tok t : r.tokens) {
    if (t.kind != Tk::eof) out.push_back(t.kind);
  }
  return out;
}

std::vector<LexDiag> diags(const pp::Preprocessed& p) {
  std::vector<LexDiag> out;
  for (const auto& d : p.diags) out.push_back(d.kind);
  return out;
}

bool clean(const pp::Preprocessed& p) { return p.diags.empty(); }

bool has(const pp::Preprocessed& p, LexDiag d) { return std::ranges::contains(diags(p), d); }

// The only token of `src`, which must lex cleanly.
Tok only(const pp::Preprocessed& p) {
  check(p.region.size() == 2, "one token");
  return p.region.tokens[0];
}

const lex::Literal& lit(const pp::Preprocessed& p, std::size_t i = 0) { return p.region.literal(p.region.tokens[i]); }

std::vector<std::uint32_t> units(std::string_view s) {
  std::vector<std::uint32_t> out;
  for (char c : s) out.push_back(static_cast<unsigned char>(c));
  return out;
}

void phases_1_and_2() {
  // Line splicing ([lex.phases]/2), including whitespace after the backslash and CR LF.
  auto p = pp::preprocess("in\\\nt x\\  \r\ny;");
  check(clean(p), "splices are clean");
  check(kinds(p.region) == std::vector{Tk::kw_int, Tk::identifier, Tk::semi}, "splices join tokens");
  check(p.region.name(p.region.tokens[1]) == "xy", "a splice inside an identifier");
  check(p.region.locs[1].offset == 6, "an identifier's offset is physical");
  check(p.region.locs[2].offset == 13, "offsets count the removed splice and CR");
  // A byte order mark is deleted; a missing final new-line is supplied.
  auto bom = pp::preprocess("\xEF\xBB\xBFint");
  check(clean(bom) && kinds(bom.region) == std::vector{Tk::kw_int}, "BOM deleted");
  check(bom.region.locs[0].offset == 3, "offset after the BOM");
  // A lone CR is a new-line: `#` after it starts a directive.
  auto cr = pp::preprocess("a\r#x\rb");
  check(diags(cr) == std::vector{LexDiag::directive_unsupported}, "lone CR ends a line");
  check(kinds(cr.region) == std::vector{Tk::identifier, Tk::identifier}, "lone CR: tokens");
  // Ill-formed UTF-8 ([lex.phases]/1).
  auto bad = pp::preprocess("a \xC3\x28 b \xED\xA0\x80 \xF0\x9F\x98");
  check(std::ranges::count(diags(bad), LexDiag::invalid_utf8) == 3, "three ill-formed sequences");
  check(bad.diags[0].offset == 2, "ill-formed UTF-8 offset");
  // Only the last backslash on a line splices; a backslash without a new-line stays.
  auto two = pp::preprocess("a\\\\\nb");
  check(kinds(two.region) == std::vector{Tk::identifier, Tk::identifier}, "\\\\ new-line");
  check(diags(two) == std::vector{LexDiag::stray_basic_character}, "the first backslash is stray");
  // Splicing can form a universal-character-name ([lex.phases]/2 note).
  auto ucn = pp::preprocess("\\u00\\\ne9");
  check(clean(ucn) && ucn.region.name(only(ucn)) == "\u00e9", "spliced UCN");
}

void comments() {
  auto p = pp::preprocess("a/* x */b // c /* d\n/* // */c");
  check(clean(p), "comments are clean");
  check(kinds(p.region) == std::vector{Tk::identifier, Tk::identifier, Tk::identifier}, "comments are whitespace");
  check(p.region.tokens[1].has(tok_flag::space_before), "a comment is a space");
  check(p.region.tokens[2].has(tok_flag::line_start), "after a line comment, a new line");
  auto u = pp::preprocess("a /* b");
  check(diags(u) == std::vector{LexDiag::unterminated_comment}, "unterminated comment");
  check(u.diags[0].offset == 2, "unterminated comment offset");
  // A new-line inside a comment does not start a line ([cpp.pre]/2).
  auto d = pp::preprocess("a /*\n*/ #x");
  check(diags(d) == std::vector{LexDiag::stray_token}, "# after a multi-line comment is not a directive");
}

void identifiers_and_keywords() {
  auto p = pp::preprocess("\u00e9 \\u00e9 \\u{E9} \\U000000E9 \\N{LATIN SMALL LETTER E WITH ACUTE}");
  check(clean(p), "UCN identifiers are clean");
  check(p.region.size() == 6, "five identifiers");
  for (std::size_t i = 0; i < 5; ++i) check(p.region.tokens[i].payload == 0, "one name for each spelling of é");
  check(p.region.names[0] == "\u00e9", "the name is UTF-8");
  // XID_Start and XID_Continue, ID_Compat_Math ([lex.name]).
  check(clean(pp::preprocess("\u03b1\u03b2 _\u0661 \u2202x x\u2082 \U0001D49C")), "Unicode identifiers");
  auto digit_start = pp::preprocess("\u0661x");
  check(has(digit_start, LexDiag::stray_character), "a digit outside ASCII cannot start an identifier");
  auto nbsp = pp::preprocess("a\u00a0b");
  check(diags(nbsp) == std::vector{LexDiag::stray_character}, "NBSP is not whitespace");
  // Normalization Form C ([lex.name]/1): e + combining acute is not NFC; é + dot below neither.
  check(has(pp::preprocess("e\u0301"), LexDiag::identifier_not_nfc), "decomposed é is not NFC");
  check(has(pp::preprocess("\u00e9\u0323"), LexDiag::identifier_not_nfc), "é + dot below is not NFC");
  check(clean(pp::preprocess("\u1EB9\u0301")), "ẹ + acute is NFC");
  check(has(pp::preprocess("\u1100\u1161"), LexDiag::identifier_not_nfc), "Hangul L+V is not NFC");
  check(clean(pp::preprocess("\uAC00")), "a Hangul syllable is NFC");
  check(has(pp::preprocess("x\u212B"), LexDiag::identifier_not_nfc), "angstrom sign is not NFC");
  // Ill-formed UCNs ([lex.universal.char]).
  check(has(pp::preprocess("\\u0041"), LexDiag::ucn_basic_or_control), "UCN of a basic character");
  check(has(pp::preprocess("a\\U00110000"), LexDiag::ucn_not_scalar), "UCN past U+10FFFF");
  check(has(pp::preprocess("\\uD800"), LexDiag::ucn_not_scalar), "UCN of a surrogate");
  check(has(pp::preprocess("\\N{NO SUCH NAME}"), LexDiag::ucn_unknown_name), "unknown character name");
  check(has(pp::preprocess("\\N{latin small letter a}"), LexDiag::ucn_unknown_name), "names match exactly");
  // Keywords ([lex.key]) and the alternative tokens ([lex.digraph]).
  auto k = pp::preprocess("int contract_assert co_await char8_t final import module");
  check(kinds(k.region) == std::vector{Tk::kw_int, Tk::kw_contract_assert, Tk::kw_co_await, Tk::kw_char8_t,
                                       Tk::identifier, Tk::identifier, Tk::identifier},
        "keywords; identifiers with special meaning stay identifiers");
  for (std::size_t i = std::to_underlying(Tk::kw_alignas); i < lex::tk_count; ++i) {
    auto one = pp::preprocess(lex::spelling(Tk(i)));
    check(kinds(one.region) == std::vector{Tk(i)}, "every keyword");
  }
  auto alt = pp::preprocess("and and_eq bitand bitor compl not not_eq or or_eq xor xor_eq");
  check(kinds(alt.region) == std::vector{Tk::ampamp, Tk::ampequal, Tk::amp, Tk::pipe, Tk::tilde, Tk::exclaim,
                                         Tk::exclaimequal, Tk::pipepipe, Tk::pipeequal, Tk::caret, Tk::caretequal},
        "alternative tokens");
  check(std::ranges::all_of(alt.region.tokens | std::views::take(11), [](Tok t) { return t.has(tok_flag::alt); }),
        "alternative tokens are flagged");
}

void punctuators() {
  // Every punctuator, on its own.
  for (auto i = std::to_underlying(Tk::l_brace); i < std::to_underlying(Tk::hash); ++i) {
    auto one = pp::preprocess(lex::spelling(Tk(i)));
    check(clean(one) && kinds(one.region) == std::vector{Tk(i)}, "every punctuator");
  }
  auto dig = pp::preprocess("<% %> <: :>");
  check(kinds(dig.region) == std::vector{Tk::l_brace, Tk::r_brace, Tk::l_square, Tk::r_square}, "digraphs");
  // Maximal munch ([lex.pptoken]/7).
  auto m = pp::preprocess("x+++++y");
  check(kinds(m.region) == std::vector{Tk::identifier, Tk::plusplus, Tk::plusplus, Tk::plus, Tk::identifier},
        "x+++++y");
  check(kinds(pp::preprocess("a<=>b->*c...d.*e^^f").region) ==
            std::vector{Tk::identifier, Tk::spaceship, Tk::identifier, Tk::arrowstar, Tk::identifier, Tk::ellipsis,
                        Tk::identifier, Tk::periodstar, Tk::identifier, Tk::caretcaret, Tk::identifier},
        "three-character punctuators");
  // The exceptions of [lex.pptoken]/4.
  check(kinds(pp::preprocess("A<::B>").region) == std::vector{Tk::identifier, Tk::less, Tk::coloncolon, Tk::identifier,
                                                              Tk::greater},
        "<:: is < ::");
  check(kinds(pp::preprocess("<:::").region) == std::vector{Tk::l_square, Tk::coloncolon}, "<::: is <: ::");
  check(kinds(pp::preprocess("<::>").region) == std::vector{Tk::l_square, Tk::r_square}, "<::> is <: :>");
  check(kinds(pp::preprocess("[::x").region) == std::vector{Tk::l_square, Tk::coloncolon, Tk::identifier},
        "[:: is [ ::");
  check(kinds(pp::preprocess("[:::").region) == std::vector{Tk::l_splice, Tk::coloncolon}, "[::: is [: ::");
  check(kinds(pp::preprocess("[:>").region) == std::vector{Tk::l_square, Tk::r_square}, "[:> is [ :>");
  check(kinds(pp::preprocess("[:x:]").region) == std::vector{Tk::l_splice, Tk::identifier, Tk::r_splice}, "splice");
  // Characters that are no token.
  for (std::string_view s : {"@", "$", "`", "\\"}) {
    check(diags(pp::preprocess(s)) == std::vector{LexDiag::stray_basic_character}, "stray basic character");
  }
}

void integer_literals() {
  auto v = [](std::string_view s) {
    auto p = pp::preprocess(s);
    check(clean(p), "integer literal is clean");
    check(only(p).kind == Tk::int_lit, "integer literal kind");
    return lit(p);
  };
  // [lex.icon]/3's example: twelve, and 1048576 five ways.
  for (std::string_view s : {"12", "014", "0XC", "0b1100"}) check(v(s).value == 12, "twelve");
  for (std::string_view s : {"1048576", "1'048'576", "0X100000", "0x10'0000", "0'004'000'000"}) {
    check(v(s).value == 1048576, "1048576");
  }
  check(v("0").base == 8 && v("10").base == 10 && v("0x1").base == 16 && v("0B1").base == 2, "bases");
  check(v("1u").int_suffix == lex::IntSuffix::u && v("1LU").int_suffix == lex::IntSuffix::ul &&
            v("1ull").int_suffix == lex::IntSuffix::ull && v("1z").int_suffix == lex::IntSuffix::z &&
            v("1Zu").int_suffix == lex::IntSuffix::uz && v("1LL").int_suffix == lex::IntSuffix::ll,
        "integer suffixes");
  check(v("18446744073709551615").value == 18446744073709551615ull, "the largest 64-bit value");
  check(has(pp::preprocess("18446744073709551616"), LexDiag::integer_too_large), "too large");
  check(has(pp::preprocess("0x1'0000'0000'0000'0000"), LexDiag::integer_too_large), "too large in hex");
  // User-defined literals ([lex.ext]): the longest literal, then an identifier.
  auto u = pp::preprocess("12_km 12LL 1f 0x 0b2x 1lL 18446744073709551616_big");
  check(clean(u), "user-defined integer literals are clean");
  auto suffix = [&](std::size_t i) {
    const auto& l = lit(u, i);
    return l.ud_suffix == lex::no_name ? std::string_view() : std::string_view(u.region.names[l.ud_suffix]);
  };
  check(suffix(0) == "_km" && lit(u, 0).value == 12, "12_km");
  check(suffix(1).empty() && lit(u, 1).int_suffix == lex::IntSuffix::ll, "12LL is not user-defined");
  check(suffix(2) == "f", "1f is 1 with ud-suffix f");
  check(suffix(3) == "x" && lit(u, 3).base == 8, "0x is 0 with ud-suffix x");
  check(lit(u, 4).base == 8 && suffix(4) == "b2x", "0b2x is 0 with ud-suffix b2x");
  check(suffix(5) == "lL", "lL is no long-long-suffix");
  check(lit(u, 6).overflow && suffix(6) == "_big", "an overflowing user-defined literal is not diagnosed");
  // Ill-formed pp-numbers ([lex.pptoken]/6's 0xe+foo; digits outside the base).
  for (std::string_view s : {"0xe+foo", "08", "0b12", "1.2.3", "0x1.2", "1e+x"}) {
    check(has(pp::preprocess(s), LexDiag::invalid_number), "invalid pp-number");
  }
  check(pp::preprocess("0xe+foo").region.size() == 2, "0xe+foo is one pp-number");
}

void floating_literals() {
  auto v = [](std::string_view s) {
    auto p = pp::preprocess(s);
    check(clean(p), "floating literal is clean");
    check(only(p).kind == Tk::float_lit, "floating literal kind");
    return lit(p);
  };
  auto d = [&](std::string_view s) { return std::bit_cast<double>(v(s).value); };
  // [lex.fcon]/2's examples.
  check(d("49.625") == 49.625 && d("0xC.68p+2") == 49.625, "49.625 and 0xC.68p+2");
  check(d("1.602'176'565e-19") == d("1.602176565e-19"), "separators");
  check(d("1E1") == 10.0 && d(".5") == 0.5 && d("1.") == 1.0 && d("1e10") == 1e10 && d("0x1p-2") == 0.25 &&
            d("0x.8p1") == 1.0 && d("0x1.p3") == 8.0,
        "forms");
  check(d("4.9406564584124654e-324") == std::numeric_limits<double>::denorm_min(), "subnormal");
  check(d("1e-400") == 0.0, "underflow to zero");
  check(std::bit_cast<float>(std::uint32_t(v("0.1f").value)) == 0.1f, "float is rounded once");
  check(has(pp::preprocess("1e309"), LexDiag::float_out_of_range), "double overflow");
  check(has(pp::preprocess("3.5e38f"), LexDiag::float_out_of_range), "float overflow");
  check(has(pp::preprocess("1000000000000000000000000000000000000000000000000e-5f"), LexDiag::float_out_of_range),
        "overflow with a negative exponent");
  check(v("1.0L").float_suffix == lex::FloatSuffix::l && v("1.0L").value == 0, "long double: the target's");
  check(v("1.0f16").float_suffix == lex::FloatSuffix::f16 && v("1.0BF16").float_suffix == lex::FloatSuffix::bf16 &&
            v("2.f128").float_suffix == lex::FloatSuffix::f128,
        "extended suffixes");
  auto u = pp::preprocess("1.2_w 1.0fl 1e 1.2e");
  check(clean(u), "user-defined floating literals are clean");
  check(u.region.names[lit(u, 0).ud_suffix] == "_w", "1.2_w");
  check(u.region.names[lit(u, 1).ud_suffix] == "fl", "1.0fl is user-defined");
  check(has(pp::preprocess("1_A\u030A"), LexDiag::identifier_not_nfc), "a pp-number's ud-suffix is checked for NFC");
  check(u.region.tokens[2].kind == Tk::int_lit && u.region.names[lit(u, 2).ud_suffix] == "e", "1e is 1 with e");
  check(u.region.tokens[3].kind == Tk::float_lit && u.region.names[lit(u, 3).ud_suffix] == "e", "1.2e is 1.2 with e");
}

void character_literals() {
  auto v = [](std::string_view s) {
    auto p = pp::preprocess(s);
    check(clean(p), "character literal is clean");
    check(only(p).kind == Tk::char_lit, "character literal kind");
    return lit(p);
  };
  check(v("'a'").value == 'a' && v("'a'").encoding == lex::Encoding::ordinary, "'a'");
  // [lex.ccon.esc], numeric escapes, UCNs.
  std::array<std::pair<std::string_view, std::uint64_t>, 18> cases{{
      {"'\\n'", 10},   {"'\\t'", 9},     {"'\\v'", 11},       {"'\\b'", 8},      {"'\\r'", 13},
      {"'\\f'", 12},   {"'\\a'", 7},     {"'\\\\'", '\\'},    {"'\\?'", '?'},    {"'\\''", '\''},
      {"'\\\"'", '"'}, {"'\\x41'", 65},  {"'\\101'", 65},     {"'\\o{101}'", 65}, {"'\\x{41}'", 65},
      {"'\\0'", 0},    {"'\\xff'", 255}, {"'\\N{LATIN SMALL LETTER A}'", 97},
  }};
  for (auto [s, x] : cases) check(v(s).value == x, "escape value");
  check(v("u8'a'").encoding == lex::Encoding::utf8, "u8 prefix");
  check(v("u'\\u00e9'").value == 0xE9 && v("u'\\u00e9'").encoding == lex::Encoding::utf16, "UTF-16 literal");
  check(v("U'\\U0001F600'").value == 0x1F600, "UTF-32 literal");
  check(v("L'\U0001F600'").value == 0x1F600, "wide literal");
  check(v("L'\\x100'").value == 0x100, "wide numeric escape");
  check(v("'x'_y").ud_suffix != lex::no_name, "user-defined character literal");
  // Ill-formed character literals.
  check(has(pp::preprocess("'\u00e9'"), LexDiag::char_not_single_unit), "é needs two UTF-8 code units");
  check(has(pp::preprocess("u8'\u00e9'"), LexDiag::char_not_single_unit), "u8 é");
  check(has(pp::preprocess("u'\U0001F600'"), LexDiag::char_not_single_unit), "u surrogate pair");
  check(has(pp::preprocess("'\\x100'"), LexDiag::escape_out_of_range), "\\x100 in char");
  check(has(pp::preprocess("u8'\\x100'"), LexDiag::escape_out_of_range), "\\x100 in char8_t");
  check(has(pp::preprocess("u'\\x10000'"), LexDiag::escape_out_of_range), "\\x10000 in char16_t");
  check(has(pp::preprocess("''"), LexDiag::empty_char), "empty");
  check(has(pp::preprocess("'a"), LexDiag::unterminated_char), "unterminated");
  check(has(pp::preprocess("'\\q'"), LexDiag::unknown_escape), "conditional escape");
  check(has(pp::preprocess("'\\x'"), LexDiag::empty_escape), "\\x without digits");
  check(has(pp::preprocess("'\\x{}'"), LexDiag::empty_escape), "\\x{} without digits");
  check(has(pp::preprocess("'\\x{41'"), LexDiag::unterminated_escape), "\\x{ without }");
  check(has(pp::preprocess("'\\o{9}'"), LexDiag::unterminated_escape), "\\o{ with a non-octal digit");
  check(has(pp::preprocess("'\\o7'"), LexDiag::unknown_escape), "\\o without {");
  check(has(pp::preprocess("'\\u12'"), LexDiag::ucn_incomplete), "short \\u");
  // Multicharacter literals ([lex.ccon]/1): conditionally supported, implementation-defined value.
  auto m = pp::preprocess("'ab'");
  check(diags(m) == std::vector{LexDiag::multichar}, "multicharacter literal is a warning");
  check(lit(m).multichar && lit(m).value == 0x6162, "multicharacter value");
  check(has(pp::preprocess("u'ab'"), LexDiag::multichar_with_prefix), "multichar with prefix");
  check(has(pp::preprocess("'a\u00e9'"), LexDiag::multichar_not_single_unit), "multichar with é");
}

void string_literals() {
  auto run = [](std::string_view s) {
    auto p = pp::preprocess(s);
    check(clean(p), "string literal is clean");
    check(p.region.tokens[0].kind == Tk::string_lit, "string literal kind");
    return lit(p);
  };
  check(run("\"abc\"").units == units("abc"), "\"abc\"");
  check(run("\"\"").units.empty(), "empty string");
  // Phase 5 ([lex.string]/8, the concatenation table).
  auto c = pp::preprocess("\"a\" \"b\" x \"c\"");
  check(c.region.tokens[1].has(tok_flag::continued) && !c.region.tokens[3].has(tok_flag::continued), "continued");
  check(lit(c).units == units("ab") && lit(c, 3).units == units("c"), "runs");
  check(run("u\"a\" \"b\"").encoding == lex::Encoding::utf16 && run("\"a\" U\"b\"").encoding == lex::Encoding::utf32 &&
            run("\"a\" L\"b\"").encoding == lex::Encoding::wide,
        "common encoding prefix");
  check(run("\"\\xA\" \"B\"").units == std::vector<std::uint32_t>{0xA, 'B'}, "\\xA B");
  check(run("R\"(\\u00)\" \"41\"").units == units("\\u0041"), "R\"(\\u00)\" \"41\"");
  check(has(pp::preprocess("u\"a\" U\"b\""), LexDiag::concat_encoding_mismatch), "two prefixes");
  check(has(pp::preprocess("\"P\"_x \"Q\" \"R\"_y"), LexDiag::concat_suffix_mismatch), "two ud-suffixes");
  auto x = pp::preprocess("L\"A\" \"B\" \"C\"_x");
  check(clean(x) && x.region.names[lit(x).ud_suffix] == "_x" && lit(x).units == units("ABC"), "L\"A\" \"B\" \"C\"_x");
  // Encodings.
  check(run("u8\"\u00e9\"").units == std::vector<std::uint32_t>{0xC3, 0xA9}, "UTF-8");
  check(run("\"\u00e9\"").units == std::vector<std::uint32_t>{0xC3, 0xA9}, "ordinary is UTF-8");
  check(run("u\"\U0001F600\"").units == std::vector<std::uint32_t>{0xD83D, 0xDE00}, "UTF-16 surrogates");
  check(run("U\"\\U0001F600\"").units == std::vector<std::uint32_t>{0x1F600}, "UTF-32");
  check(run("\"\\N{LATIN CAPITAL LETTER GHA}\"").units == std::vector<std::uint32_t>{0xC6, 0xA2}, "a name alias");
  check(run("\"\\u0041\"").units == units("A"), "a basic UCN inside a literal is fine");
  check(run("\"\\xff\"").units == std::vector<std::uint32_t>{0xFF}, "numeric escape is one code unit");
  check(has(pp::preprocess("\"\\x100\""), LexDiag::escape_out_of_range), "\\x100 in char");
  check(has(pp::preprocess("\"a"), LexDiag::unterminated_string), "unterminated");
  // Raw strings ([lex.string]/4–6's examples).
  check(run("R\"delimiter((a|b))delimiter\"").units == units("(a|b)"), "raw delimiter");
  check(run("R\"(a\\\nb\nc)\"").units == units("a\\\nb\nc"), "raw string reverts splices");
  check(run("R\"a(\n)\\\na\"\n)a\"").units == units("\n)\\\na\"\n"), "raw string with )\\ and a\"");
  check(run("R\"(x = \"\\\"y\\\"\")\"").units == units("x = \"\\\"y\\\"\""), "raw string with quotes");
  check(run("u8R\"x(\u00e9)x\"").units == std::vector<std::uint32_t>{0xC3, 0xA9}, "u8R");
  check(run("LR\"(a)\"_s").ud_suffix != lex::no_name, "user-defined raw string");
  check(has(pp::preprocess("R\"0123456789abcdefg(x)0123456789abcdefg\""), LexDiag::raw_delimiter_too_long),
        "17-character delimiter");
  check(clean(pp::preprocess("R\"0123456789abcdef(x)0123456789abcdef\"")), "16-character delimiter");
  check(has(pp::preprocess("R\"a b(x)a b\""), LexDiag::raw_delimiter_bad_character), "space in a delimiter");
  auto sp = pp::preprocess("R\"a\\\nb(x)a\\\nb\"");
  check(!sp.diags.empty() && sp.diags[0].kind == LexDiag::raw_delimiter_bad_character && sp.diags[0].offset == 3,
        "a delimiter's backslash inside a reverted splice is located at itself");
  check(has(pp::preprocess("R\"(abc"), LexDiag::unterminated_raw_string), "unterminated raw string");
  // [lex.pptoken]/5: R"y" is an ill-formed raw string, not R and "y".
  check(has(pp::preprocess("R\"y\";"), LexDiag::raw_delimiter_bad_character), "R\"y\"");
  // An identifier followed by a quote is not a prefix.
  check(kinds(pp::preprocess("x\"a\" u8 \"b\"").region) ==
            std::vector{Tk::identifier, Tk::string_lit, Tk::identifier, Tk::string_lit},
        "not a prefix");
}

void directives() {
  auto p = pp::preprocess("#define X 1\nint x;\n  # include <a>\n%:x\n#\n");
  check(std::ranges::count(diags(p), LexDiag::directive_unsupported) == 4, "four directives");
  check(kinds(p.region) == std::vector{Tk::kw_int, Tk::identifier, Tk::semi}, "directive lines are skipped");
  check(p.diags[0].offset == 0, "directive offset");
  check(diags(pp::preprocess("a # b")) == std::vector{LexDiag::stray_token}, "# inside a line");
  check(diags(pp::preprocess("a ## b")) == std::vector{LexDiag::stray_token}, "##");
  // [cpp.pre]/2's examples.
  auto is_directive = [](std::string_view s) { return has(pp::preprocess(s), LexDiag::directive_unsupported); };
  check(is_directive("module ;"), "module ;");
  check(is_directive("export module leftpad;"), "export module leftpad;");
  check(is_directive("import <string>;"), "import <string>;");
  check(is_directive("export import \"squee\";"), "export import \"squee\";");
  check(is_directive("import rightpad;"), "import rightpad;");
  check(is_directive("import :part;"), "import :part;");
  check(!is_directive("module\n;"), "module new-line ;");
  check(!is_directive("export\nimport\nfoo;"), "export new-line import new-line foo;");
  check(is_directive("export\nimport foo;"), "export new-line import foo;");
  check(!is_directive("import ::"), "import ::");
  check(!is_directive("import ->"), "import ->");
  check(!is_directive("x import foo;"), "import not at the start of a line");
  auto rest = pp::preprocess("export\nimport foo;\nint");
  check(kinds(rest.region) == std::vector{Tk::kw_export, Tk::kw_int}, "export stays a token");
  // A header-name is formed only on `import`'s line: here `import` is a variable ([lex.pptoken]/4).
  auto v = pp::preprocess("int f(int import, int a, int b) { return\nimport\n<a> b; }");
  check(clean(v) && v.region.tokens[15].kind == Tk::less, "no header-name after a line break");
  check(clean(pp::preprocess("int f(int import) { return\nimport\n\"x\"[0]; }")), "nor a quoted one");
  // A directive's line ends at the new-line, after a multi-line comment too.
  auto c = pp::preprocess("#x /*\n*/ y\nz");
  check(kinds(c.region) == std::vector{Tk::identifier}, "a comment does not end a directive");
}

void locations_and_flags() {
  auto p = pp::preprocess("a  b\n  c");
  check(p.region.tokens[0].has(tok_flag::line_start) && !p.region.tokens[0].has(tok_flag::space_before), "first");
  check(p.region.tokens[1].has(tok_flag::space_before) && !p.region.tokens[1].has(tok_flag::line_start), "second");
  check(p.region.tokens[2].has(tok_flag::line_start) && p.region.tokens[2].has(tok_flag::space_before), "third");
  check(p.region.locs[2].offset == 7 && p.region.locs[3].offset == 8, "offsets and the end");
  check(p.region.tokens.back().kind == Tk::eof, "eof");
  check(p.region.spelling(p.region.tokens[0]) == "a", "spelling");
  // The same text gives the same region, and the names are numbered by first appearance.
  check(pp::preprocess("b a b").region.names == std::vector<std::string>{"b", "a"}, "names in order");
}

void rendering() {
  check(lex::render(LexDiag::stray_character, std::array<std::uint64_t, 1>{0xA0}) ==
            "character \u00a0 (U+00A0) is not allowed here",
        "render a code point");
  check(lex::render(LexDiag::stray_token, std::array<std::uint64_t, 1>{std::to_underlying(Tk::hash)}) ==
            "stray '#' in program",
        "render a token kind");
  check(lex::lex_diag_of(lex::diag_id(LexDiag::multichar)) == LexDiag::multichar, "DiagId round trip");
  check(lex::message_of(LexDiag::multichar).severity == lex::Severity::warning, "severity");
  check(lex::diag_name(LexDiag::invalid_utf8) == "invalid_utf8", "name");
}

void unicode_names() {
  using lex::unicode::lookup_name;
  check(lookup_name("LATIN SMALL LETTER A") == U'a', "a name");
  check(lookup_name("ABACUS") == U'\U0001F9EE', "the first name");
  check(lookup_name("CJK UNIFIED IDEOGRAPH-4E00") == U'\u4E00', "an algorithmic name");
  check(!lookup_name("CJK UNIFIED IDEOGRAPH-0041"), "an algorithmic prefix with another code point");
  check(lookup_name("HANGUL SYLLABLE GAG") == U'\uAC01' && lookup_name("HANGUL SYLLABLE A") == U'\uC544',
        "Hangul syllables");
  check(lookup_name("LINE FEED") == U'\n' && lookup_name("BYTE ORDER MARK") == U'\uFEFF', "aliases");
  check(!lookup_name("LATIN SMALL LETTER") && !lookup_name("") && !lookup_name("ZZZZ"), "not names");
}

// `pp` as an engine query: its value, its diagnostics, and early cutoff on an edit.
struct Count {
  using Key = pp::FileId;
  using Value = std::size_t;
  static engine::query_task<Value> run(engine::Ctx& cx, Key f) { co_return (co_await cx.fetch<pp::Pp>(f)).size(); }
};

void query() {
  engine::Db db;
  pp::FileId f{7};
  db.set<pp::FileText>(f, "int main() { return 42; } @");
  const Region& r = db.get<pp::Pp>(f);
  check(r.size() == 10 && r.files == std::vector{f}, "pp(file)");
  const auto& d = db.diags_of<pp::Pp>(f);
  check(d.size() == 1 && d[0].at.item == lex::file_level && d[0].at.offset == 26 &&
            d[0].id == lex::diag_id(LexDiag::stray_basic_character),
        "pp's diagnostics");
  check(db.get<Count>(f) == 10, "a query over pp");
  // A comment changes the region's locations but not its token count: Count is cut off.
  db.set<pp::FileText>(f, "int main() { return 42; } /**/ @");
  db.reset_stats();
  check(db.get<Count>(f) == 10, "after an edit");
  check(db.runs<pp::Pp>(f) == 2 && db.runs<Count>(f) == 2, "pp re-ran; Count re-ran on the new region");
  db.set<pp::FileText>(f, "int main() { return 42; } /**/ @");
  db.reset_stats();
  db.get<Count>(f);
  check(db.stats().executed == 0, "an equal text changes nothing");
}

}  // namespace

int main() {
  phases_1_and_2();
  comments();
  identifiers_and_keywords();
  punctuators();
  integer_literals();
  floating_literals();
  character_literals();
  string_literals();
  directives();
  locations_and_flags();
  rendering();
  unicode_names();
  query();
  return ycxx::unit::result("unit.lex");
}
