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
  // Before token `at`, `-E` continues the line of `start` through that of `end` (in one file).
  struct Continuation {
    u32 at = 0;
    u16 file = 0;
    u32 start = 0;
    u32 end = 0;
  };
  std::vector<Continuation> continuations;
  FileId main{};
  // The files included as system headers, whose phase 5 and 6 warnings are not reported (#229).
  // By file: a header without a guard included both as a system header and not is taken for one
  // in both places.
  std::set<FileId> system;
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

  Bindings root(const std::vector<RootBinding>& defs) {
    Bindings b;
    for (const RootBinding& d : defs) b.emplace_back(file(d.file, d.file_name), d.offset);
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
    if (r.system) out_->system.insert(r.files[0]);
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
    std::size_t inc = 0, pass = 0, diag = 0, cont = 0;
    for (std::size_t k = 0; k <= r.tokens.size(); ++k) {
      for (; cont < r.continuations.size() && r.continuations[cont].at == k; ++cont)
        out_->continuations.push_back(
            {static_cast<u32>(out_->tokens.size()), files(0), r.continuations[cont].start, r.continuations[cont].end});
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
  }
  // The command line's diagnostics come first, as it is preprocessed first.
  if (t.builtins && (!t.builtins->diags.empty() || !t.builtins->tokens.empty())) fl.add(*t.builtins, fl.root(t.builtins_bindings));
  if (t.main) fl.add(*t.main, fl.root(t.main_bindings));
  return out;
}

namespace detail {

// Track only whether natural record layout is certain. This does not apply pragma packing
// to layout or diagnose the directive. Unknown extensions can affect both the current
// setting and the saved settings; an explicit reset establishes the default again.
class PackPragmas {
 public:
  PackPragmas(Session& s, const Flat& flat, Region& region) : s_(&s), flat_(&flat), region_(&region) {}

  void advance(std::size_t i) {
    while (next_ < flat_->passthrough.size() && flat_->passthrough[next_].first <= i) {
      const auto& line = flat_->passthrough[next_++].second;
      if (line.size() < 3 || !name(line[2], "pack")) continue;
      bool before = capped_;
      change(std::span(line).subspan(3));
      auto at = u32(region_->tokens.size());
      if (!before && capped_) begin_ = at;
      if (before && !capped_) close(at);
    }
  }

  void finish() {
    if (capped_) close(u32(region_->tokens.size()));
  }

 private:
  Session* s_;
  const Flat* flat_;
  Region* region_;
  std::size_t next_ = 0;
  u32 begin_ = 0;
  bool capped_ = false, unknown_stack_ = false;
  std::vector<std::uint8_t> saved_;

  bool name(const PTok& p, std::string_view value) const {
    return p.tok.kind == Tk::identifier && s_->spellings.name(p.tok.payload) == value;
  }

  std::optional<bool> cap(const PTok& p) const {
    if (p.tok.kind != Tk::pp_number) return std::nullopt;
    std::string_view n = s_->spellings.literal(p.tok.payload).spelling;
    if (n == "0") return false;
    if (n == "1" || n == "2" || n == "4" || n == "8" || n == "16") return true;
    return std::nullopt;
  }

  void close(u32 at) {
    if (begin_ != at) region_->possibly_packed.emplace_back(begin_, at);
  }

  void change(std::span<const PTok> tokens) {
    if (tokens.size() >= 2 && tokens.front().tok.kind == Tk::l_paren && tokens.back().tok.kind == Tk::r_paren) {
      auto args = tokens.subspan(1, tokens.size() - 2);
      if (args.empty()) {
        capped_ = false;
        return;
      }
      if (args.size() == 1) {
        if (auto setting = cap(args[0])) capped_ = *setting;
        else if (name(args[0], "push")) saved_.push_back(capped_);
        else if (name(args[0], "pop")) {
          if (saved_.empty()) capped_ = capped_ || unknown_stack_;
          else {
            capped_ = saved_.back();
            saved_.pop_back();
          }
        } else {
          unknown();
        }
        return;
      }
      if (args.size() == 3 && name(args[0], "push") && args[1].tok.kind == Tk::comma) {
        if (auto setting = cap(args[2])) {
          saved_.push_back(capped_);
          capped_ = *setting;
          return;
        }
      }
    }
    unknown();
  }

  void unknown() {
    capped_ = true;
    unknown_stack_ = true;
    saved_.clear();
  }
};

}  // namespace detail

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
      Diag at{pp[i].loc.file, pp[i].loc.offset + offset, d, {a, b}, 0, std::nullopt};
      // A warning is hidden where its token's outermost expansion is in a system header, as Clang
      // and GCC decide. One spelled in a system header's macro and expanded elsewhere is reported
      // where the macro is used.
      if (hidden_in_system_header(d)) {
        TokLoc l = pp[i].loc;
        while (l.expansion != 0) {
          const Expansion& e = flat.expansions[l.expansion];
          l = TokLoc{e.site_file, e.parent, e.site_offset};
        }
        if (flat.system.contains(flat.files[l.file])) return;
        if (flat.system.contains(flat.files[at.file])) {
          at.file = l.file;
          at.offset = l.offset;
        }
      }
      diags.emplace_back(u64(i) * 2 + 1, at);
    };
  };
  auto literal = [&](u32 id) {
    Literal l = s.spellings.literal(id);
    if (l.ud_suffix != lex::no_name) l.ud_suffix = tables.name(s.spellings.name(l.ud_suffix));
    return tables.literal(std::move(l));
  };
  auto emit = [&](Tok t, const PTok& p) {
    t.flags &= u8(tok_flag::line_start | tok_flag::space_before | tok_flag::alt | tok_flag::continued | tok_flag::directive);
    r.tokens.push_back(t);
    r.locs.push_back(p.loc);
  };
  detail::PackPragmas pack(s, flat, r);
  std::array<std::optional<u32>, 256> byte_literals;
  for (std::size_t i = 0; i < pp.size(); ++i) {
    pack.advance(i);
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
    case Tk::embed: {
      // Materialize ordinary parser tokens so embeds work wherever an integer or comma
      // is allowed, including a single literal in an expression.
      bool first = true;
      for (unsigned char byte : s.spellings.resource(tok.payload)) {
        if (!first) emit(Tok{Tk::comma, 0, 0}, p);
        if (!byte_literals[byte]) {
          Literal l;
          l.spelling = std::to_string(byte);
          l.value = byte;
          l.base = byte == 0 ? 8 : 10;
          byte_literals[byte] = tables.literal(std::move(l));
        }
        emit(Tok{Tk::int_lit, first ? tok.flags : u8(0), *byte_literals[byte]}, p);
        first = false;
      }
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
        pack.advance(k);
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
  pack.finish();
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
  // The line of the last `)` of the invocations printed on this line: tokens up to it stay here.
  u32 cont_line = 0;
  auto marker = [&](u16 file, u32 line) {
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += std::format("# {} \"{}\"\n", line, s.spellings.name(flat.file_names[file]));
    cur_file = file;
    cur_line = line;
    cont_line = 0;
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
  auto close_line = [&](TokLoc l) {
    u32 best = 0;
    while (l.expansion != 0) {
      const Expansion& e = flat.expansions[l.expansion];
      if (e.kind == ExpansionKind::function && e.operand_a != 0) {
        TokLoc outer = site(l);
        best = std::max(best, line_of(outer.file, e.operand_a - 1).first);
      }
      l = TokLoc{e.site_file, e.parent, e.site_offset};
    }
    return best;
  };
  // Continuations waiting for the printer to reach their invocation's line: file, start, end.
  struct Armed {
    u16 file;
    u32 start, end;
  };
  std::vector<Armed> armed;
  auto fire = [&] {
    std::erase_if(armed, [&](const Armed& a) {
      if (static_cast<int>(a.file) != cur_file || a.start > cur_line) return false;
      if (a.start == cur_line) cont_line = std::max(cont_line, a.end);
      return true;
    });
  };
  std::size_t pass = 0;
  bool after_embed = false;
  u32 joined_line = 0;
  bool prev_resource = false;
  auto print_tok = [&](const PTok& t, bool directive_start) {
    TokLoc at = site(t.loc);
    auto [line, column] = line_of(at.file, at.offset);
    u32 expansion = t.loc.expansion;
    while (expansion != 0 && flat.expansions[expansion].kind != ExpansionKind::embed)
      expansion = flat.expansions[expansion].parent;
    const bool embed = expansion != 0;
    const bool resource = t.tok.kind == Tk::embed;
    const bool join = !directive_start && static_cast<int>(at.file) == cur_file &&
                      (embed || after_embed || line == joined_line);
    const bool begins_after_embed = !embed && after_embed && join;
    if (begins_after_embed) joined_line = line;
    else if (!join) joined_line = 0;
    if (directive_start) {
      // A directive starts its own line; in the middle of one (a _Pragma), as Clang prints it,
      // after a marker for the same line.
      if (!line_empty || static_cast<int>(at.file) != cur_file || line != cur_line) marker(at.file, line);
    } else if (join) {
      // Clang prints an embed's replacement at the current output position, followed
      // by the first following source line before resuming normal line placement.
    } else if (static_cast<int>(at.file) == cur_file && line > cur_line && line <= cont_line) {
      // Still on the line of an invocation that ended on a later line, as Clang prints it.
    } else if (static_cast<int>(at.file) != cur_file || line != cur_line) {
      if (static_cast<int>(at.file) == cur_file && line > cont_line && cont_line > cur_line) cur_line = cont_line;
      move_to(at.file, line);
    }
    fire();
    std::string_view bytes = resource ? std::as_const(s.spellings).resource(t.tok.payload) : std::string_view{};
    std::string text = resource ? std::to_string(static_cast<unsigned char>(bytes.front())) : spelling(t.tok, s.spellings);
    if (line_empty) {
      if (!directive_start && !embed) out.append(column > 1 ? column - 1 : 0, ' ');
    } else {
      bool adjacent = t.loc.expansion == 0 && prev_loc.expansion == 0 && t.loc.file == prev_loc.file &&
                      t.loc.offset == prev_end;
      bool space = (!resource && t.tok.has(tok_flag::space_before)) ||
                   (!resource && prev_resource && (embed || begins_after_embed));
      if (!space && !adjacent) space = detail::separate(prev2, prev, text);
      if (space) out += ' ';
    }
    out += text;
    for (unsigned char byte : resource ? bytes.substr(1) : std::string_view{}) {
      text = std::to_string(byte);
      out += ", ";
      out += text;
    }
    after_embed = embed;
    prev_resource = resource;
    line_empty = false;
    prev2 = std::exchange(prev, text);
    prev_loc = t.loc;
    prev_end = t.loc.offset + text.size();
    if (static_cast<int>(at.file) == cur_file) cont_line = std::max(cont_line, close_line(t.loc));
    if (cont_line < cur_line) cont_line = 0;
  };
  std::size_t boundary = 0, cont = 0;
  for (std::size_t i = 0; i <= flat.tokens.size(); ++i) {
    for (; cont < flat.continuations.size() && flat.continuations[cont].at == i; ++cont) {
      const Flat::Continuation& c = flat.continuations[cont];
      armed.push_back({c.file, line_of(c.file, c.start).first, line_of(c.file, c.end).first});
    }
    fire();  // an invocation on the current line (an empty one leaves no token to fire it)
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
