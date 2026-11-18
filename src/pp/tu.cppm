// A translation unit's tree flattened (`Flat`), then either converted by phases 5 and 6 into the
// `Region` the item splitter and the parser read, or printed as `-E` output.
export module ycxx.pp:tu;

import std;
import ycxx.hash;
import ycxx.lex;
import :spell;
import :files;
import :region;
import :unit;

export namespace ycxx::pp {

// The tree's tokens in order, with the TU's file and expansion tables. Directive lines `-E`
// passes through and diagnostics sit before the token whose index they carry.
struct Flat {
  std::vector<PTok> tokens;
  std::vector<FileId> files;
  std::vector<u32> file_names;  // presumed names, `Spellings` ids
  std::vector<Expansion> expansions{Expansion{}};
  std::vector<std::pair<u32, std::vector<PTok>>> passthrough;
  std::vector<std::pair<u32, Diag>> diags;
  std::vector<u32> boundaries;  // where a region starts or resumes after an include
  FileId main{};
};

// What preprocessing a translation unit produced: the region and every diagnostic, in order.
struct Preprocessed {
  Region region;
  std::vector<Diag> diags;
};

namespace detail {

class Flattener {
public:
  Flattener(Session& s, Flat& out) : s_(&s), out_(&out) {}

  // Where a region's bindings are in the TU: a file of its table, and the definition's offset.
  using Bindings = std::vector<std::pair<u16, u32>>;

  Bindings root(const std::vector<std::pair<FileId, u32>>& defs) {
    Bindings b;
    for (auto [f, offset] : defs) b.emplace_back(file(f, s_->spellings.name(s_->files.path(f))), offset);
    return b;
  }

  void first(const FileRegion& r) { file(r.files[0], r.file_names[0]); }

  void add(const FileRegion& r, const Bindings& bound) {
    // The region's files in the TU's table, added when first used.
    std::vector<std::optional<u16>> local(r.files.size());
    auto files = [&](u16 i) {
      if (!local[i]) local[i] = file(r.files[i], r.file_names[i]);
      return *local[i];
    };
    files(0);
    const u32 base = static_cast<u32>(out_->expansions.size());
    auto exp = [&](u32 local) { return local == 0 ? 0 : base + local - 1; };
    // A location in the TU's tables; a bound one is relative to its definition.
    auto place = [&](u16 f, u32 offset) {
      if (!is_bound(f)) return std::pair{files(f), offset};
      auto [tf, at] = bound[f & ~bound_file];
      return std::pair{tf, at + offset};
    };
    for (std::size_t i = 1; i < r.expansions.size(); ++i) {
      Expansion e = r.expansions[i];
      e.parent = exp(e.parent);
      std::tie(e.site_file, e.site_offset) = place(e.site_file, e.site_offset);
      std::tie(e.definition_file, e.definition_offset) = place(e.definition_file, e.definition_offset);
      out_->expansions.push_back(e);
    }
    auto map = [&](TokLoc l) {
      auto [f, offset] = place(l.file, l.offset);
      return TokLoc{f, exp(l.expansion), offset};
    };
    // Includes, passed-through lines and diagnostics are placed by their offsets in the file
    // among the tokens, which are placed by their sites.
    auto site = [&](std::size_t k) {
      TokLoc l = r.tokens[k].loc;
      while (l.expansion != 0) {
        const Expansion& e = r.expansions[l.expansion];
        l = TokLoc{e.site_file, e.parent, e.site_offset};
      }
      return l.offset;
    };
    std::size_t inc = 0, pass = 0, diag = 0;
    for (std::size_t k = 0; k <= r.tokens.size(); ++k) {
      const u32 limit = k < r.tokens.size() ? site(k) : std::numeric_limits<u32>::max();
      for (;;) {
        // The earliest pending event at this position.
        u32 best = std::numeric_limits<u32>::max();
        int which = -1;
        if (inc < r.includes.size() && r.includes[inc].at == k && r.includes[inc].offset < best) {
          best = r.includes[inc].offset;
          which = 0;
        }
        if (pass < r.passthrough.size() && r.passthrough[pass].at == k && r.passthrough[pass].tokens[0].loc.offset < best) {
          best = r.passthrough[pass].tokens[0].loc.offset;
          which = 1;
        }
        if (diag < r.diags.size() && (r.diags[diag].offset < limit || k == r.tokens.size()) && r.diags[diag].offset <= best) {
          best = r.diags[diag].offset;
          which = 2;
        }
        if (which < 0) break;
        if (which == 0) {
          if (r.includes[inc].region) {
            Bindings child;
            for (const Bound& b : r.includes[inc].bindings)
              child.push_back(b.outer ? bound[b.binding] : std::pair{files(b.file), b.offset});
            out_->boundaries.push_back(static_cast<u32>(out_->tokens.size()));
            add(*r.includes[inc].region, child);
            out_->boundaries.push_back(static_cast<u32>(out_->tokens.size()));
          }
          ++inc;
        } else if (which == 1) {
          std::vector<PTok> line = r.passthrough[pass].tokens;
          for (PTok& t : line) t.loc = map(t.loc);
          out_->passthrough.emplace_back(static_cast<u32>(out_->tokens.size()), std::move(line));
          ++pass;
        } else {
          Diag d = r.diags[diag];
          d.file = files(d.file);
          out_->diags.emplace_back(static_cast<u32>(out_->tokens.size()), d);
          ++diag;
        }
      }
      if (k < r.tokens.size()) {
        PTok t = r.tokens[k];
        t.loc = map(t.loc);
        out_->tokens.push_back(t);
      }
    }
  }

private:
  u16 file(FileId f, u32 name) {
    auto [it, fresh] = index_.emplace(std::pair{f, name}, u16(out_->files.size()));
    if (fresh) {
      out_->files.push_back(f);
      out_->file_names.push_back(name);
    }
    return it->second;
  }

  Session* s_;
  Flat* out_;
  std::map<std::pair<FileId, u32>, u16> index_;
};

}  // namespace detail

Flat flatten(Session& s, const Tree& t) {
  Flat out;
  detail::Flattener fl(s, out);
  if (t.main) {
    out.main = t.main->file;
    fl.first(*t.main);  // the main file is [0]
    fl.add(*t.main, fl.root(t.main_bindings));
  }
  if (t.builtins && (!t.builtins->diags.empty() || !t.builtins->tokens.empty())) fl.add(*t.builtins, fl.root(t.builtins_bindings));
  return out;
}

// Phases 5 and 6 ([lex.phases]) over the flattened tokens: string-literal concatenation, then
// pp-tokens become tokens (keywords, classified numbers, evaluated character literals).
Preprocessed to_region(Session& s, const Flat& flat) {
  Preprocessed out;
  Region& r = out.region;
  lex::Tables tables;
  // Diagnostics keep their place: those of phase 4 before the token they precede, those of
  // phases 5 and 6 at their token.
  std::vector<std::pair<u64, Diag>> diags;
  for (const auto& [at, d] : flat.diags) diags.emplace_back(u64(at) * 2, d);
  const std::vector<PTok>& pp = flat.tokens;
  auto report_at = [&](std::size_t i) {
    return [&, i](u32 offset, lex::LexDiag d, u64 a, u64 b) {
      diags.emplace_back(u64(i) * 2 + 1, Diag{pp[i].loc.file, pp[i].loc.offset + offset, d, {a, b}});
    };
  };
  auto literal = [&](u32 id) {
    Literal l = s.spellings.literal(id);
    if (l.ud_suffix != lex::no_name) l.ud_suffix = tables.name(s.spellings.name(l.ud_suffix));
    return tables.literal(std::move(l));
  };
  auto emit = [&](Tok t, const PTok& p) {
    t.flags &= u8(tok_flag::line_start | tok_flag::space_before | tok_flag::alt | tok_flag::continued);
    r.tokens.push_back(t);
    r.locs.push_back(p.loc);
  };
  for (std::size_t i = 0; i < pp.size(); ++i) {
    const PTok& p = pp[i];
    Tok tok = p.tok;
    switch (tok.kind) {
    case Tk::identifier: {
      std::string_view name = s.spellings.name(tok.payload);
      if (auto k = lex::keyword(name)) tok = Tok{*k, tok.flags, 0};
      else tok.payload = tables.name(name);
      emit(tok, p);
      break;
    }
    case Tk::pp_number:
      tok.payload = literal(tok.payload);
      tok.kind = lex::classify_number(tables.literals[tok.payload], tables, report_at(i));
      emit(tok, p);
      break;
    case Tk::char_lit:
      tok.payload = literal(tok.payload);
      lex::evaluate_char(tables.literals[tok.payload], report_at(i));
      emit(tok, p);
      break;
    case Tk::string_lit: {
      // Phase 5: the run of adjacent string literals.
      std::size_t j = i;
      while (j + 1 < pp.size() && pp[j + 1].tok.kind == Tk::string_lit) ++j;
      std::vector<u32> ids;
      for (std::size_t k = i; k <= j; ++k) ids.push_back(literal(pp[k].tok.payload));
      std::vector<Literal*> run;
      for (u32 id : ids) run.push_back(&tables.literals[id]);
      lex::concatenate_strings(std::span<Literal*>(run), [&](std::size_t k, u32 offset, lex::LexDiag d, u64 a, u64 b) {
        report_at(i + k)(offset, d, a, b);
      });
      for (std::size_t k = i; k <= j; ++k) {
        Tok t = pp[k].tok;
        t.payload = ids[k - i];
        if (k > i) t.flags |= tok_flag::continued;
        emit(t, pp[k]);
      }
      i = j;
      break;
    }
    case Tk::pp_other:
      // [lex.pptoken]/3. Characters outside the basic character set (controls included) were
      // reported in phase 3.
      if (tok.payload >= 0x20 && tok.payload < 0x7F && tok.payload != '\'' && tok.payload != '"')
        report_at(i)(0, lex::LexDiag::stray_basic_character, tok.payload, 0);
      break;
    case Tk::hash:
    case Tk::hashhash: report_at(i)(0, lex::LexDiag::stray_token, std::to_underlying(tok.kind), 0); break;
    case Tk::header_name:
      // Formed only where a directive reads it.
      contract_assert(false);
      break;
    default: emit(tok, p); break;
    }
  }
  const Files::Content* main = s.files.content(flat.main);
  r.tokens.push_back(Tok{Tk::eof, tok_flag::line_start, 0});
  r.locs.push_back({0, 0, main ? static_cast<u32>(main->text.size()) : 0});
  r.names = std::move(tables.names);
  r.literals = std::move(tables.literals);
  for (FileId f : flat.files) r.files.push_back(s.files.path(f));
  for (u32 n : flat.file_names) r.file_names.emplace_back(s.spellings.name(n));
  r.expansions = flat.expansions;
  for (std::size_t i = 1; i < r.expansions.size(); ++i) {
    u32 name = r.expansions[i].macro;
    auto it = std::ranges::find(r.names, s.spellings.name(name));
    r.expansions[i].macro = static_cast<u32>(it - r.names.begin());
    if (it == r.names.end()) r.names.emplace_back(s.spellings.name(name));
  }
  std::ranges::stable_sort(diags, {}, &std::pair<u64, Diag>::first);
  for (auto& [key, d] : diags) out.diags.push_back(d);
  return out;
}

// Line starts of a file's physical text: after LF, CR LF and a lone CR.
std::vector<u32> line_starts(std::string_view text) {
  std::vector<u32> starts{0};
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n' || (text[i] == '\r' && (i + 1 == text.size() || text[i + 1] != '\n')))
      starts.push_back(static_cast<u32>(i + 1));
  }
  return starts;
}

namespace detail {

// Whether `next` printed right after `tail` would be lexed as another token: then `-E` separates
// them with a space.
bool would_paste(std::string_view tail, std::string_view next) {
  std::string text(tail);
  text += next;
  std::vector<lex::LexDiagnostic> ignored;
  lex::Source src(text, ignored);
  lex::Tables tables;
  lex::Lexer lx(src, tables, ignored);
  for (;;) {
    lex::PpToken t = lx.next();
    if (t.tok.kind == Tk::eof || t.begin > tail.size()) return true;
    if (t.begin == tail.size()) return false;
    if (t.end > tail.size()) return true;
  }
}

// Whether `-E` separates `next` from `prev` (after `prev2`) when they were not adjacent in the
// source: where they would paste, as Clang's output has it. Clang also separates a number from a
// sign (which could extend it after an exponent letter) and a third `.` after two, but not a
// number from a character literal.
bool separate(std::string_view prev2, std::string_view prev, std::string_view next) {
  bool number = !prev.empty() && ((prev[0] >= '0' && prev[0] <= '9') || (prev[0] == '.' && prev.size() > 1));
  if (number && (next == "+" || next == "-")) return true;
  if (number && next.starts_with('\'')) return false;
  if (prev2 == "." && prev == "." && next.starts_with('.')) return true;
  return would_paste(prev, next);
}

}  // namespace detail

// `-E` output: each token on the line of its site, separated from the one before where it had
// whitespace or would otherwise paste; a line marker where the file changes or lines are skipped.
std::string print(Session& s, const Flat& flat) {
  std::string out;
  std::map<u16, std::vector<u32>> lines;
  auto line_of = [&](u16 file, u32 offset) {
    auto it = lines.find(file);
    if (it == lines.end()) {
      const Files::Content* c = s.files.content(flat.files[file]);
      it = lines.emplace(file, line_starts(c ? std::string_view(c->text) : std::string_view())).first;
    }
    auto at = std::ranges::upper_bound(it->second, offset);
    u32 line = static_cast<u32>(at - it->second.begin());
    return std::pair{line, offset - *(at - 1) + 1};
  };
  auto site = [&](TokLoc l) {
    while (l.expansion != 0) {
      const Expansion& e = flat.expansions[l.expansion];
      l = TokLoc{e.site_file, e.parent, e.site_offset};
    }
    return l;
  };
  int cur_file = -1;
  u32 cur_line = 0;
  bool line_empty = true;
  std::string prev, prev2;  // the last two tokens printed
  TokLoc prev_loc;
  std::size_t prev_end = 0;
  auto marker = [&](u16 file, u32 line) {
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += std::format("# {} \"{}\"\n", line, s.spellings.name(flat.file_names[file]));
    cur_file = file;
    cur_line = line;
    line_empty = true;
  };
  auto move_to = [&](u16 file, u32 line) {
    if (static_cast<int>(file) != cur_file || line < cur_line || line > cur_line + 8) {
      marker(file, line);
    } else {
      while (cur_line < line) {
        out += '\n';
        ++cur_line;
        line_empty = true;
      }
    }
  };
  std::size_t pass = 0;
  auto print_tok = [&](const PTok& t, bool directive_start) {
    TokLoc at = site(t.loc);
    auto [line, column] = line_of(at.file, at.offset);
    if (directive_start) {
      move_to(at.file, line);
      if (!line_empty) {
        out += '\n';
        ++cur_line;
      }
    } else if (static_cast<int>(at.file) != cur_file || line != cur_line) {
      move_to(at.file, line);
    }
    std::string text = spelling(t.tok, s.spellings);
    if (line_empty) {
      if (!directive_start) out.append(column > 1 ? column - 1 : 0, ' ');
    } else {
      bool adjacent = t.loc.expansion == 0 && prev_loc.expansion == 0 && t.loc.file == prev_loc.file &&
                      t.loc.offset == prev_end;
      bool space = t.tok.has(tok_flag::space_before);
      if (!space && !adjacent) space = detail::separate(prev2, prev, text);
      if (space) out += ' ';
    }
    out += text;
    line_empty = false;
    prev2 = std::exchange(prev, text);
    prev_loc = t.loc;
    prev_end = t.loc.offset + text.size();
  };
  std::size_t boundary = 0;
  for (std::size_t i = 0; i <= flat.tokens.size(); ++i) {
    for (; boundary < flat.boundaries.size() && flat.boundaries[boundary] == i; ++boundary) cur_file = -1;
    for (; pass < flat.passthrough.size() && flat.passthrough[pass].first == i; ++pass) {
      const std::vector<PTok>& line = flat.passthrough[pass].second;
      for (std::size_t k = 0; k < line.size(); ++k) print_tok(line[k], k == 0);
      out += '\n';
      ++cur_line;
      line_empty = true;
    }
    if (i < flat.tokens.size()) print_tok(flat.tokens[i], false);
  }
  if (!out.empty() && out.back() != '\n') out += '\n';
  return out;
}

}  // namespace ycxx::pp
