// ycxx.pp: the `pp` query (query-engine.md §B9) without directives yet (#40). It runs translation
// phases 1–3 (ycxx.lex), diagnoses every preprocessing directive as unsupported in place of
// phase 4, and runs phases 5 and 6, so its value is a region of tokens ready for the item
// splitter and the parser.
//
// The region is shaped for M1's preprocessor, which adds to it rather than changing it: include
// references among the tokens, the macro delta, the include list, the guard summary, `#line`
// directives and macro expansions (ast.md §8.1, §8.6). The key gains the prefix of §B9 when
// contextual queries (#16) exist.
export module ycxx.pp;

import std;
import ycxx.engine;
export import ycxx.lex;

export namespace ycxx::pp {

using lex::Literal;
using lex::Tk;
using lex::Tok;
using lex::u16;
using lex::u32;

// A file's identity, assigned by whoever sets its text (the driver).
enum class FileId : u32 {};

// The location of a token (ast.md §8.1): the byte offset of its spelling in a file of the region's
// file table, and the innermost macro expansion that produced it (0: none; there are none yet).
struct TokLoc {
  u16 file = 0;
  u32 expansion = 0;
  u32 offset = 0;
  bool operator==(const TokLoc&) const = default;
};

// The value of `pp(file)`: the file's tokens after phase 6 and what they refer to.
struct Region {
  // The tokens, then one `Tk::eof`. Kinds are never pp-only. A string literal flagged
  // `tok_flag::continued` belongs to the run that starts at the nearest earlier unflagged one,
  // which holds the run's value.
  std::vector<Tok> tokens;
  // Parallel to `tokens`; the eof's offset is the file's size.
  std::vector<TokLoc> locs;
  // Identifier and ud-suffix spellings in UTF-8, universal-character-names replaced; an
  // identifier's payload indexes them.
  std::vector<std::string> names;
  // A literal's payload indexes them.
  std::vector<Literal> literals;
  // The file table `TokLoc::file` indexes; [0] is the region's own file.
  std::vector<FileId> files;
  bool operator==(const Region&) const = default;

  std::size_t size() const { return tokens.size(); }
  // An identifier's spelling.
  std::string_view name(Tok t) const
    pre(t.kind == Tk::identifier)
  {
    return names[t.payload];
  }
  const Literal& literal(Tok t) const
    pre(lex::is_literal(t.kind))
  {
    return literals[t.payload];
  }
  // A token's spelling: an identifier's name, a literal's spelling, a keyword's or punctuator's
  // primary spelling.
  std::string_view spelling(Tok t) const {
    if (t.kind == Tk::identifier) return names[t.payload];
    if (lex::is_literal(t.kind)) return literals[t.payload].spelling;
    return lex::spelling(t.kind);
  }
};

// What preprocessing one file produced: the region and the diagnostics, located by byte offset.
struct Preprocessed {
  Region region;
  std::vector<lex::LexDiagnostic> diags;
};

// Phases 1–6 of one file, as a function (the query's body, and what tests call).
Preprocessed preprocess(std::string_view text, FileId file = FileId{0});

// The text of a file: an input, set by the driver.
struct [[=engine::input{engine::Durability::low}]] FileText {
  using Key = FileId;
  using Value = std::string;
};

// `pp(file)` (§B9). Its diagnostics are at `Loc{lex::file_level, byte offset}`.
struct Pp {
  using Key = FileId;
  using Value = Region;
  static engine::query_task<Value> run(engine::Ctx& cx, Key file);
};

}  // namespace ycxx::pp

namespace ycxx::pp {

using lex::LexDiag;
using lex::PpToken;
namespace tok_flag = lex::tok_flag;

// Phase 4, for now: directives are recognized ([cpp.pre]/2) and reported as unsupported.
class Directives {
public:
  Directives(lex::Lexer& lx, lex::Tables& tables, std::vector<lex::LexDiagnostic>& diags, const lex::Source& src)
      : lx_(&lx), tables_(&tables), diags_(&diags), src_(&src) {}

  // The next token that is not part of a directive.
  PpToken next() {
    for (;;) {
      PpToken t = take();
      if (t.tok.kind == Tk::eof || !t.tok.has(tok_flag::line_start) || !introduces_directive(t)) return t;
      diags_->push_back({src_->physical(t.begin), LexDiag::directive_unsupported, {}});
      lx_->skip_line();
      pending_.clear();
    }
  }

private:
  PpToken take(bool header_name = false) {
    if (!pending_.empty()) {
      PpToken t = pending_.front();
      pending_.pop_front();
      return t;
    }
    return header_name ? lx_->next_header_name() : lx_->next();
  }
  // Looks at the token after the ones already pending, without consuming anything.
  const PpToken& peek(std::size_t n, bool header_name = false) {
    while (pending_.size() <= n) pending_.push_back(header_name ? lx_->next_header_name() : lx_->next());
    return pending_[n];
  }
  bool is_name(const PpToken& t, std::string_view s) const {
    return t.tok.kind == Tk::identifier && tables_->names[t.tok.payload] == s;
  }
  static bool same_line(const PpToken& t) { return t.tok.kind != Tk::eof && !t.tok.has(tok_flag::line_start); }
  // [cpp.pre]/2: `#`, or `import`, `module` or `export` followed on the same line by the right
  // token.
  bool introduces_directive(const PpToken& t) {
    if (t.tok.kind == Tk::hash) return true;
    std::size_t at = 0;
    PpToken word = t;
    if (is_name(t, "export")) {
      const PpToken& n = peek(0);
      if (!same_line(n) || !(is_name(n, "import") || is_name(n, "module"))) return false;
      word = n;
      at = 1;
    }
    if (is_name(word, "import")) {
      const PpToken& n = peek(at, true);
      return same_line(n) && (n.tok.kind == Tk::header_name || n.tok.kind == Tk::less ||
                              n.tok.kind == Tk::identifier || n.tok.kind == Tk::colon);
    }
    if (is_name(word, "module")) {
      const PpToken& n = peek(at);
      return same_line(n) && (n.tok.kind == Tk::identifier || n.tok.kind == Tk::colon || n.tok.kind == Tk::semi);
    }
    return false;
  }

  lex::Lexer* lx_;
  lex::Tables* tables_;
  std::vector<lex::LexDiagnostic>* diags_;
  const lex::Source* src_;
  std::deque<PpToken> pending_;
};

}  // namespace ycxx::pp

export namespace ycxx::pp {

Preprocessed preprocess(std::string_view text, FileId file) {
  Preprocessed out;
  std::vector<lex::LexDiagnostic>& diags = out.diags;
  lex::Source src(text, diags);
  lex::Tables tables;
  lex::Lexer lx(src, tables, diags);
  Directives dirs(lx, tables, diags, src);
  std::vector<PpToken> pp;
  for (PpToken t = dirs.next(); t.tok.kind != Tk::eof; t = dirs.next()) pp.push_back(t);

  Region& r = out.region;
  auto report_at = [&](u32 logical) {
    return [&, logical](u32 offset, LexDiag d, std::uint64_t a, std::uint64_t b) {
      diags.push_back({src.physical(logical + offset), d, {a, b}});
    };
  };
  auto emit = [&](Tok t, u32 begin) {
    r.tokens.push_back(t);
    r.locs.push_back({0, 0, src.physical(begin)});
  };
  for (std::size_t i = 0; i < pp.size(); ++i) {
    PpToken t = pp[i];
    Tok tok = t.tok;
    switch (tok.kind) {
    case Tk::identifier:
      if (auto k = lex::keyword(tables.names[tok.payload])) tok = Tok{*k, tok.flags, 0};
      emit(tok, t.begin);
      break;
    case Tk::pp_number: {
      tok.kind = lex::classify_number(tables.literals[tok.payload], tables, report_at(t.begin));
      emit(tok, t.begin);
      break;
    }
    case Tk::char_lit:
      lex::evaluate_char(tables.literals[tok.payload], report_at(t.begin));
      emit(tok, t.begin);
      break;
    case Tk::string_lit: {
      // Phase 5: the run of adjacent string literals.
      std::size_t j = i;
      while (j + 1 < pp.size() && pp[j + 1].tok.kind == Tk::string_lit) ++j;
      std::vector<Literal*> run;
      for (std::size_t k = i; k <= j; ++k) run.push_back(&tables.literals[pp[k].tok.payload]);
      lex::concatenate_strings(std::span<Literal*>(run), [&](std::size_t k, u32 offset, LexDiag d, std::uint64_t a,
                                                              std::uint64_t b) {
        diags.push_back({src.physical(pp[i + k].begin + offset), d, {a, b}});
      });
      for (std::size_t k = i; k <= j; ++k) {
        Tok s = pp[k].tok;
        if (k > i) s.flags |= tok_flag::continued;
        emit(s, pp[k].begin);
      }
      i = j;
      break;
    }
    case Tk::pp_other:
      // [lex.pptoken]/3. Characters outside the basic character set (controls included) were
      // reported in phase 3.
      if (tok.payload >= 0x20 && tok.payload < 0x7F && tok.payload != '\'' && tok.payload != '"') {
        diags.push_back({src.physical(t.begin), LexDiag::stray_basic_character, {tok.payload, 0}});
      }
      break;
    case Tk::hash:
    case Tk::hashhash:
      diags.push_back({src.physical(t.begin), LexDiag::stray_token, {std::to_underlying(tok.kind), 0}});
      break;
    case Tk::header_name:
      // Formed only after `import`, which always introduces a directive here.
      contract_assert(false);
      break;
    default:
      emit(tok, t.begin);
      break;
    }
  }
  r.tokens.push_back(Tok{Tk::eof, tok_flag::line_start, 0});
  r.locs.push_back({0, 0, static_cast<u32>(text.size())});
  r.names = std::move(tables.names);
  r.literals = std::move(tables.literals);
  r.files = {file};
  std::ranges::stable_sort(diags, {}, &lex::LexDiagnostic::offset);
  return out;
}

engine::query_task<Region> Pp::run(engine::Ctx& cx, FileId file) {
  Preprocessed p = preprocess(cx.fetch<FileText>(file), file);
  for (const lex::LexDiagnostic& d : p.diags) {
    cx.diag(engine::Loc{lex::file_level, d.offset}, lex::diag_id(d.kind), d.args[0], d.args[1]);
  }
  co_return std::move(p.region);
}

}  // namespace ycxx::pp
