// Translation phase 3 ([lex.phases]/3, [lex.pptoken]): the logical text becomes preprocessing
// tokens. Universal-character-names outside literals are replaced by the characters they name,
// comments become whitespace, and each token records whether it starts a line or follows
// whitespace. Literal tokens are recorded with their spelling only; their values are computed
// after phase 4 (:literal), where adjacent string literals are known.
export module ycxx.lex:lexer;

import std;
import :tok;
import :diag;
import :unicode;
import :source;

export namespace ycxx::lex {

inline constexpr u32 no_name = 0xffffffffu;

// A literal token's entry (char, string, integer and floating literals, and pp-numbers).
//
// What is filled when:
// - the lexer sets `spelling` (the logical spelling; a raw string's has phase 2 reverted),
//   `ud_suffix`, `encoding`, `raw` and `bad`;
// - phases 5 and 6 (:literal) set the rest.
struct Literal {
  std::string spelling;
  u32 ud_suffix = no_name;  // the names index of the ud-suffix ([lex.ext]), or no_name
  Encoding encoding = Encoding::ordinary;
  bool raw = false;  // a raw string literal
  bool bad = false;  // ill-formed and diagnosed: has no value
  // [lex.icon]: base 2, 8, 10 or 16; `value` is the value, `overflow` set if it exceeds 64 bits.
  // [lex.fcon]: base 10 or 16; `value` holds the IEEE bits of the value for the suffixes none
  // (binary64) and f (binary32), and is 0 for the others, whose formats are the target's.
  // [lex.ccon]: `value` is the code unit, or the numeric escape's value before conversion to the
  // literal's type, or a multicharacter literal's int value (as two's-complement bits).
  u8 base = 0;
  IntSuffix int_suffix = IntSuffix::none;
  FloatSuffix float_suffix = FloatSuffix::none;
  bool overflow = false;
  bool multichar = false;
  u64 value = 0;
  // [lex.string]: on the first string literal of a run that phase 5 concatenates, the code units
  // of the whole run without the terminating null, and the run's common encoding and ud-suffix;
  // the run's other tokens are flagged `tok_flag::continued`.
  std::vector<u32> units;
  bool operator==(const Literal&) const = default;
};

// The identifier spellings and literals that tokens index. Indices are assigned in order of
// first appearance, so they are a function of the text alone.
class Tables {
public:
  std::vector<std::string> names;
  std::vector<Literal> literals;

  u32 name(std::string_view s) {
    auto it = index_.find(s);
    if (it != index_.end()) return it->second;
    u32 n = static_cast<u32>(names.size());
    names.emplace_back(s);
    index_.emplace(std::string(s), n);
    return n;
  }
  u32 literal(Literal l) {
    literals.push_back(std::move(l));
    return static_cast<u32>(literals.size() - 1);
  }

private:
  struct string_hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
  };
  std::unordered_map<std::string, u32, string_hash, std::equal_to<>> index_;
};

// A preprocessing token and its logical extent.
struct PpToken {
  Tok tok;
  u32 begin = 0;
  u32 end = 0;
};

// The UCN at `s[i]` ([lex.universal.char]): nothing if there is none; its length, and its value
// or the reason it is ill-formed.
struct UcnParse {
  char32_t value = 0;
  u32 length = 0;
  std::optional<LexDiag> error;
};
std::optional<UcnParse> parse_ucn(std::string_view s, std::size_t i);

class Lexer {
public:
  Lexer(const Source& src, Tables& tables, std::vector<LexDiagnostic>& diags)
      : src_(&src), text_(src.text()), tables_(&tables), diags_(&diags) {}

  // The next preprocessing token; `Tk::eof` at the end, forever.
  PpToken next();
  // The next preprocessing token where a header-name may be formed ([lex.pptoken]/4).
  PpToken next_header_name();
  // Skips to the end of the current logical line; the next token starts a line.
  void skip_line();

  // `parse_ucn` at a logical offset, reporting an ill-formed UCN.
  std::optional<UcnParse> ucn(u32 at);

  void report(u32 logical, LexDiag kind, u64 a = 0, u64 b = 0) {
    diags_->push_back({src_->physical(logical), kind, {a, b}});
  }

private:
  u8 skip_whitespace();
  Tok lex();
  Tok identifier(bool alternatives = true);
  Tok pp_number();
  Tok quoted(u32 prefix_begin, u32 quote, Encoding enc);
  Tok raw_string(u32 prefix_begin, u32 quote, Encoding enc);
  Tok punctuator_or_other();
  u32 ud_suffix();

  // An identifier character at `at`, if one is there: its value and length.
  struct IdChar {
    char32_t c;
    u32 length;
    bool ucn;
  };
  std::optional<IdChar> id_char(u32 at, bool start);
  char at(u32 i) const { return i < text_.size() ? text_[i] : '\0'; }

  const Source* src_;
  std::string_view text_;
  Tables* tables_;
  std::vector<LexDiagnostic>* diags_;
  u32 pos_ = 0;
  bool line_start_ = true;
};

}  // namespace ycxx::lex

namespace ycxx::lex {

struct Punct {
  std::string_view text;
  Tk kind;
  bool alt;
};

// The punctuators, longest first: the primary spellings from `Tk`, then the digraphs.
constexpr auto punctuators = [] {
  constexpr std::size_t primaries = std::to_underlying(Tk::hashhash) - std::to_underlying(Tk::l_brace) + 1;
  std::array<Punct, primaries + 6> t{};
  std::size_t n = 0;
  for (auto k = std::to_underlying(Tk::l_brace); k <= std::to_underlying(Tk::hashhash); ++k) {
    t[n++] = {spelling(Tk(k)), Tk(k), false};
  }
  t[n++] = {"<%", Tk::l_brace, true};
  t[n++] = {"%>", Tk::r_brace, true};
  t[n++] = {"<:", Tk::l_square, true};
  t[n++] = {":>", Tk::r_square, true};
  t[n++] = {"%:%:", Tk::hashhash, true};
  t[n++] = {"%:", Tk::hash, true};
  std::ranges::stable_sort(t, std::ranges::greater{}, [](const Punct& p) { return p.text.size(); });
  return t;
}();

constexpr bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
constexpr u32 hex_value(char c) { return c <= '9' ? u32(c - '0') : (c | 0x20) - 'a' + 10; }
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_nondigit(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

}  // namespace ycxx::lex

export namespace ycxx::lex {

std::optional<UcnParse> parse_ucn(std::string_view s, std::size_t i) {
  auto at = [&](std::size_t j) { return j < s.size() ? s[j] : '\0'; };
  if (at(i) != '\\') return std::nullopt;
  char k = at(i + 1);
  if ((k == 'u' || k == 'U') && !(k == 'u' && at(i + 2) == '{')) {
    std::size_t digits = k == 'u' ? 4 : 8;
    u64 v = 0;
    for (std::size_t d = 0; d < digits; ++d) {
      if (!is_hex(at(i + 2 + d))) return std::nullopt;
      v = v * 16 + hex_value(at(i + 2 + d));
    }
    UcnParse u{char32_t(std::min<u64>(v, 0xFFFFFFFF)), u32(digits + 2), {}};
    if (v > 0x10FFFF || !unicode::is_scalar_value(char32_t(v))) u.error = LexDiag::ucn_not_scalar;
    return u;
  }
  if (k == 'u' || k == 'N') {
    std::size_t j = i + 3;
    if (at(i + 2) != '{') return std::nullopt;
    while (j < s.size() && s[j] != '}' && s[j] != '\n' && (k == 'N' || is_hex(s[j]))) ++j;
    if (at(j) != '}' || j == i + 3) return std::nullopt;
    UcnParse u{0, u32(j + 1 - i), {}};
    if (k == 'N') {
      auto c = unicode::lookup_name(s.substr(i + 3, j - i - 3));
      if (c) u.value = *c;
      else u.error = LexDiag::ucn_unknown_name;
      return u;
    }
    u64 v = 0;
    for (std::size_t d = i + 3; d < j; ++d) v = std::min<u64>(v * 16 + hex_value(s[d]), 0xFFFFFFFF);
    u.value = char32_t(v);
    if (!unicode::is_scalar_value(u.value)) u.error = LexDiag::ucn_not_scalar;
    return u;
  }
  return std::nullopt;
}

std::optional<UcnParse> Lexer::ucn(u32 i) {
  auto u = parse_ucn(text_, i);
  if (u && u->error) report(i, *u->error, u->value);
  return u;
}

PpToken Lexer::next() {
  u8 flags = skip_whitespace();
  u32 begin = pos_;
  if (pos_ >= text_.size()) return {Tok{Tk::eof, flags, 0}, begin, begin};
  Tok t = lex();
  t.flags |= flags;
  return {t, begin, pos_};
}

PpToken Lexer::next_header_name() {
  u8 flags = skip_whitespace();
  u32 begin = pos_;
  char open = at(pos_);
  if (open == '<' || open == '"') {
    char close = open == '<' ? '>' : '"';
    u32 j = pos_ + 1;
    while (j < text_.size() && text_[j] != close && text_[j] != '\n') ++j;
    if (at(j) == close && j > pos_ + 1) {
      pos_ = j + 1;
      Tok t{Tk::header_name, flags, tables_->name(text_.substr(begin, pos_ - begin))};
      return {t, begin, pos_};
    }
  }
  if (pos_ >= text_.size()) return {Tok{Tk::eof, flags, 0}, begin, begin};
  Tok t = lex();
  t.flags |= flags;
  return {t, begin, pos_};
}

void Lexer::skip_line() {
  // Comments still delimit: a `/*` comment that spans lines ends the directive's line after it.
  while (pos_ < text_.size()) {
    u8 flags = skip_whitespace();
    if ((flags & tok_flag::line_start) != 0 || pos_ >= text_.size()) {
      line_start_ = true;
      return;
    }
    lex();
  }
}

u8 Lexer::skip_whitespace() {
  bool space = false;
  while (pos_ < text_.size()) {
    char c = text_[pos_];
    if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
      space = true;
      ++pos_;
    } else if (c == '\n') {
      line_start_ = true;
      space = false;
      ++pos_;
    } else if (c == '/' && at(pos_ + 1) == '/') {
      while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
      space = true;
    } else if (c == '/' && at(pos_ + 1) == '*') {
      std::size_t end = text_.find("*/", pos_ + 2);
      if (end == text_.npos) {
        report(pos_, LexDiag::unterminated_comment);
        pos_ = static_cast<u32>(text_.size());
      } else {
        pos_ = static_cast<u32>(end + 2);
      }
      space = true;
    } else {
      break;
    }
  }
  u8 flags = (line_start_ ? tok_flag::line_start : 0) | (space ? tok_flag::space_before : 0);
  line_start_ = false;
  return flags;
}

Tok Lexer::lex() {
  u32 begin = pos_;
  char c = text_[pos_];
  // Encoding prefixes and raw strings ([lex.ccon], [lex.string]). A raw string wins over an
  // identifier ([lex.pptoken]/4.1).
  if (c == 'u' || c == 'U' || c == 'L' || c == 'R') {
    u32 j = pos_;
    Encoding enc = Encoding::ordinary;
    if (c == 'u' && at(j + 1) == '8') {
      enc = Encoding::utf8;
      j += 2;
    } else if (c != 'R') {
      enc = c == 'u' ? Encoding::utf16 : c == 'U' ? Encoding::utf32 : Encoding::wide;
      j += 1;
    }
    bool raw = at(j) == 'R';
    if (raw) ++j;
    if (at(j) == '"' && (raw || j > pos_)) return raw ? raw_string(begin, j, enc) : quoted(begin, j, enc);
    if (at(j) == '\'' && !raw && j > pos_) return quoted(begin, j, enc);
  }
  if (c == '\'' || c == '"') return quoted(begin, pos_, Encoding::ordinary);
  if (is_digit(c) || (c == '.' && is_digit(at(pos_ + 1)))) return pp_number();
  if (id_char(pos_, true)) return identifier();
  return punctuator_or_other();
}

std::optional<Lexer::IdChar> Lexer::id_char(u32 i, bool start) {
  unsigned char c = static_cast<unsigned char>(at(i));
  if (c < 0x80 && c != '\\') {
    bool ok = start ? unicode::is_identifier_start(c) : unicode::is_identifier_continue(c);
    if (ok) return IdChar{c, 1, false};
    return std::nullopt;
  }
  if (c == '\\') {
    // Peek without reporting: the identifier lexer reports when it consumes.
    auto u = parse_ucn(text_, i);
    if (!u) return std::nullopt;
    if (u->error) return IdChar{0xFFFD, u->length, true};  // ill-formed; keep it in the identifier
    char32_t v = u->value;
    if (v < 0xA0) return IdChar{v, u->length, true};  // a basic or control character: ill-formed
    bool ok = start ? unicode::is_identifier_start(v) : unicode::is_identifier_continue(v);
    if (ok) return IdChar{v, u->length, true};
    return std::nullopt;
  }
  auto d = unicode::decode_utf8(text_.substr(i));
  if (!d) return std::nullopt;
  bool ok = start ? unicode::is_identifier_start(d->c) : unicode::is_identifier_continue(d->c);
  if (ok) return IdChar{d->c, d->length, false};
  return std::nullopt;
}

Tok Lexer::identifier(bool alternatives) {
  u32 begin = pos_;
  std::string name;
  std::u32string cps;
  bool ascii = true;
  for (bool start = true;; start = false) {
    auto ch = id_char(pos_, start);
    if (!ch) break;
    if (ch->ucn) {
      ucn(pos_);  // reports an ill-formed one
      if (ch->c < 0xA0) report(pos_, LexDiag::ucn_basic_or_control, ch->c);
    }
    unicode::append_utf8(name, ch->c);
    cps += ch->c;
    ascii = ascii && ch->c < 0x80;
    pos_ += ch->length;
  }
  if (!ascii && !unicode::is_nfc(cps)) report(begin, LexDiag::identifier_not_nfc);
  if (alternatives) {
    if (auto alt = alternative_token(name)) return Tok{*alt, tok_flag::alt, 0};
  }
  return Tok{Tk::identifier, 0, tables_->name(name)};
}

Tok Lexer::pp_number() {
  u32 begin = pos_;
  pos_ += text_[pos_] == '.' ? 2 : 1;
  for (;;) {
    char c = at(pos_);
    char n = at(pos_ + 1);
    if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (n == '+' || n == '-')) {
      pos_ += 2;
    } else if (c == '\'' && (is_digit(n) || is_nondigit(n))) {
      pos_ += 2;
    } else if (c == '.') {
      pos_ += 1;
    } else if (auto ch = id_char(pos_, false)) {
      if (ch->ucn) {
        ucn(pos_);
        if (ch->c < 0xA0) report(pos_, LexDiag::ucn_basic_or_control, ch->c);
      }
      pos_ += ch->length;
    } else {
      break;
    }
  }
  Literal l;
  l.spelling = text_.substr(begin, pos_ - begin);
  return Tok{Tk::pp_number, 0, tables_->literal(std::move(l))};
}

u32 Lexer::ud_suffix() {
  if (!id_char(pos_, true)) return no_name;
  return identifier(false).payload;
}

Tok Lexer::quoted(u32 begin, u32 quote, Encoding enc) {
  char q = text_[quote];
  u32 j = quote + 1;
  bool bad = false;
  for (;;) {
    char c = at(j);
    if (c == '\n' || j >= text_.size()) {
      report(quote, q == '\'' ? LexDiag::unterminated_char : LexDiag::unterminated_string);
      bad = true;
      break;
    }
    if (c == '\\') {
      j += at(j + 1) == '\n' ? 1 : 2;
      continue;
    }
    ++j;
    if (c == q) break;
  }
  if (!bad && q == '\'' && j == quote + 2) {
    report(quote, LexDiag::empty_char);
    bad = true;
  }
  pos_ = j;
  Literal l;
  l.encoding = enc;
  l.bad = bad;
  if (!bad) l.ud_suffix = ud_suffix();
  l.spelling = text_.substr(begin, pos_ - begin);
  return Tok{q == '\'' ? Tk::char_lit : Tk::string_lit, 0, tables_->literal(std::move(l))};
}

Tok Lexer::raw_string(u32 begin, u32 quote, Encoding enc) {
  std::string_view p1 = src_->phase1();
  const u32 open = src_->to_phase1(quote) + 1;  // the first d-char, in phase-1 text
  Literal l;
  l.encoding = enc;
  l.raw = true;
  u32 d = 0;
  for (;; ++d) {
    char c = open + d < p1.size() ? p1[open + d] : '\n';
    if (c == '(') break;
    bool basic = static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F;
    if (!basic || c == ' ' || c == ')' || c == '\\' || d == 16) {
      if (d == 16) report(src_->to_logical(open + d), LexDiag::raw_delimiter_too_long);
      else report(src_->to_logical(open + d), LexDiag::raw_delimiter_bad_character, static_cast<unsigned char>(c));
      l.bad = true;
      pos_ = src_->to_logical(open + d);
      l.spelling = text_.substr(begin, pos_ - begin);
      return Tok{Tk::string_lit, 0, tables_->literal(std::move(l))};
    }
  }
  std::string closing = ")";
  closing += p1.substr(open, d);
  closing += '"';
  std::size_t found = p1.find(closing, open + d + 1);
  u32 end_p1 = 0;
  if (found == p1.npos) {
    report(quote, LexDiag::unterminated_raw_string);
    l.bad = true;
    end_p1 = static_cast<u32>(p1.size());
    pos_ = static_cast<u32>(text_.size());
  } else {
    end_p1 = static_cast<u32>(found + closing.size());
    pos_ = src_->to_logical(end_p1);
  }
  l.spelling = text_.substr(begin, quote + 1 - begin);
  l.spelling += p1.substr(open, end_p1 - open);
  if (!l.bad) {
    u32 suffix_begin = pos_;
    l.ud_suffix = ud_suffix();
    l.spelling += text_.substr(suffix_begin, pos_ - suffix_begin);
  }
  return Tok{Tk::string_lit, 0, tables_->literal(std::move(l))};
}

Tok Lexer::punctuator_or_other() {
  std::string_view rest = text_.substr(pos_);
  // [lex.pptoken]/4.2 and 4.3: `<::` and `[::` and `[:>`.
  if (rest.starts_with("<::") && at(pos_ + 3) != ':' && at(pos_ + 3) != '>') {
    pos_ += 1;
    return Tok{Tk::less, 0, 0};
  }
  if ((rest.starts_with("[::") && at(pos_ + 3) != ':') || rest.starts_with("[:>")) {
    pos_ += 1;
    return Tok{Tk::l_square, 0, 0};
  }
  for (const Punct& p : punctuators) {
    if (rest.starts_with(p.text)) {
      pos_ += static_cast<u32>(p.text.size());
      return Tok{p.kind, p.alt ? tok_flag::alt : u8(0), 0};
    }
  }
  // A single character that is no other preprocessing token. A universal-character-name names
  // its character ([lex.phases]/3). Outside the basic character set it is ill-formed here
  // ([lex.pptoken]/1); basic ones are ill-formed only if they survive to phase 6.
  u32 begin = pos_;
  char32_t c = static_cast<unsigned char>(text_[pos_]);
  u32 length = 1;
  bool known = true;
  if (auto u = ucn(pos_)) {
    length = u->length;
    known = !u->error;
    c = known ? u->value : 0xFFFD;
    if (known && c < 0xA0) {
      report(begin, LexDiag::ucn_basic_or_control, c);
      known = false;
    }
  } else if (c >= 0x80) {
    if (auto d = unicode::decode_utf8(rest)) {
      c = d->c;
      length = d->length;
    } else {
      known = false;  // diagnosed in phase 1
    }
  }
  pos_ += length;
  if (!known) c = 0xFFFD;  // reported already
  bool basic = c == '\t' || c == '\v' || c == '\f' || (c >= 0x20 && c < 0x7F);
  if (known && !basic) report(begin, LexDiag::stray_character, c);
  return Tok{Tk::pp_other, 0, static_cast<u32>(c)};
}

}  // namespace ycxx::lex
