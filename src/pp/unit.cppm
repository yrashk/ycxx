// Phase 4 of one translation unit ([cpp]): the main file and what it includes, as a tree of
// `FileRegion`s, each either computed or reused from the session's cache when its trace verifies
// against the current state (query-engine.md §B9).
//
// State and questions. The state is the macro table, the `#pragma once` set and, later, the
// `__COUNTER__` value. Every read of it goes through `ask`, which records the question in each
// region being computed whose own history (its directives and those of the regions it has
// included so far) did not decide the answer: those are the questions its entry state answered.
// A region's reads of the file system (include resolutions, contents) are recorded in every
// region being computed.
//
// Expansion ([cpp.replace], [cpp.rescan]) uses a stack of contexts per file: each macro
// replacement pushes its tokens and disables the macro until they are read; an identifier read
// while its macro is disabled is painted and never replaced. A file's end is a barrier: no
// expansion reads past it.
export module ycxx.pp:unit;

import std;
import ycxx.hash;
import ycxx.lex;
import :spell;
import :files;
import :region;

export namespace ycxx::pp {

// What preprocessing shares across translation units: spellings, files, the configuration and
// the regions computed so far.
class Session {
public:
  explicit Session(Host& host, Config cfg = {}) : files(host), config(std::move(cfg)), config_fp(config.fingerprint()) {}
  Session(const Session&) = delete;

  Spellings spellings;
  Files files;
  const Config config;
  const Fp128 config_fp;

  // Regions by file occurrence, each entry valid where its trace verifies.
  struct Key {
    FileId file{};
    u32 presumed = 0;
    u32 found_dir = 0;
    Fp128 content;
    auto operator<=>(const Key&) const = default;
  };
  std::map<Key, std::vector<std::shared_ptr<const FileRegion>>> cache;

  // How many regions were computed and how many reused, for tests and status.
  struct Stats {
    u32 computed = 0;
    u32 reused = 0;
  } stats;

  // The predefined macros and the command line's, as the text of `<built-in>` ([cpp.predefined]).
  std::string builtin_text() const {
    std::string s = "#define __cplusplus 202400L\n";
    for (const auto& [name, value] : config.macros) {
      if (value) s += std::format("#define {} {}\n", name, *value);
      else s += std::format("#undef {}\n", name);
    }
    return s;
  }
};

// A translation unit's regions: the predefined macros, then the main file, each with where its
// bindings' definitions are.
struct Tree {
  std::shared_ptr<const FileRegion> builtins;
  std::shared_ptr<const FileRegion> main;
  std::vector<std::pair<FileId, u32>> builtins_bindings;
  std::vector<std::pair<FileId, u32>> main_bindings;
};

inline constexpr std::size_t max_include_depth = 200;

class Unit {
public:
  explicit Unit(Session& s) : s_(&s) {}

  Tree run(FileId main, std::string_view presumed) {
    Tree t;
    FileId b = s_->files.id("<built-in>");
    s_->files.set_content(b, s_->builtin_text());
    t.builtins = enter_root(b, "<built-in>", t.builtins_bindings);
    if (!fatal_) t.main = enter_root(main, presumed, t.main_bindings);
    return t;
  }

private:
  struct Context {
    std::vector<PTok> toks;
    std::size_t pos = 0;
    std::optional<u32> macro;  // disabled while its tokens are read
    bool barrier = false;      // reading past it gives eof
  };

  struct Frame {
    FileId file{};
    u32 found_dir = includer_dir;
    std::string here;  // the directory of the real path
    std::string presumed;
    std::unique_ptr<lex::Source> src;
    lex::Tables tables;  // the lexer's scratch tables; tokens are moved to the session's
    std::vector<lex::LexDiagnostic> lex_diags;
    std::unique_ptr<lex::Lexer> lexer;
    std::deque<PTok> pending;
    std::vector<Context> contexts;
    std::unordered_map<u32, u32> active;  // disabled macros
    bool pending_space = false;
    FileRegion out;
    std::map<FileId, u16> file_index;
    // What this region's own history decided, and what it has asked already.
    std::unordered_set<u32> touched;
    std::set<FileId> once_touched;
    std::unordered_set<u32> asked_macro;
    std::set<FileId> asked_once;
    std::set<FileId> asked_content;
    std::vector<Question> asked_include;
    // Bindings: by name, and where each is as the includer sees it (the TU, for a root).
    std::unordered_map<u32, u32> binding_index;
    std::vector<Bound> parent_bounds;
    std::vector<std::pair<FileId, u32>> root_bindings;
  };

  // --- regions ---------------------------------------------------------------------------------

  std::shared_ptr<const FileRegion> enter_root(FileId file, std::string_view presumed, std::vector<std::pair<FileId, u32>>& bindings) {
    const Files::Content* c = s_->files.content(file);
    if (!c) return nullptr;
    std::vector<Bound> ignored;
    return enter(file, presumed, includer_dir, *c, ignored, &bindings);
  }

  // The region of `file` here: a cached one whose trace verifies, or a new one. `bounds` gets its
  // bindings as the current region sees them, or `root` as the TU does.
  std::shared_ptr<const FileRegion> enter(FileId file, std::string_view presumed, u32 found_dir, const Files::Content& c,
                                          std::vector<Bound>& bounds, std::vector<std::pair<FileId, u32>>* root = nullptr) {
    Session::Key key{file, s_->spellings.name(presumed), found_dir, c.fp};
    if (auto it = s_->cache.find(key); it != s_->cache.end()) {
      for (const std::shared_ptr<const FileRegion>& r : it->second) {
        if (verifies(*r)) {
          // Its bindings are the entry state's definitions.
          for (u32 name : r->bindings) {
            const MacroDef& d = *macros_.at(name);
            if (root) root->emplace_back(d.file, d.offset);
            else bounds.push_back(bound_for(frames_.size() - 1, name, d));
          }
          reuse(*r);
          ++s_->stats.reused;
          return r;
        }
      }
    }
    auto f = std::make_unique<Frame>();
    f->file = file;
    f->found_dir = found_dir;
    f->here = directory_of(s_->files.path(file));
    f->presumed = presumed;
    f->src = std::make_unique<lex::Source>(c.text, f->lex_diags);
    f->lexer = std::make_unique<lex::Lexer>(*f->src, f->tables, f->lex_diags);
    f->out.file = file;
    f->out.presumed = key.presumed;
    f->out.content = c.fp;
    f->out.files = {file};
    f->out.file_names = {key.presumed};
    f->file_index.emplace(file, u16(0));
    frames_.push_back(std::move(f));
    process(*frames_.back());
    std::shared_ptr<const FileRegion> r = finish(*frames_.back());
    if (root) *root = std::move(frames_.back()->root_bindings);
    else bounds = std::move(frames_.back()->parent_bounds);
    frames_.pop_back();
    ++s_->stats.computed;
    if (!r->fatal) s_->cache[key].push_back(r);
    return r;
  }

  bool verifies(const FileRegion& r) {
    for (const Question& q : r.trace) {
      if (answer(q) != q.answer) return false;
    }
    return true;
  }

  // The answer the current state gives to a question.
  Fp128 answer(const Question& q) {
    switch (q.ask) {
    case Ask::macro: {
      auto it = macros_.find(q.a);
      return it == macros_.end() ? Fp128{} : it->second->fp;
    }
    case Ask::once: return Fp128{once_.contains(FileId{q.a}) ? 1u : 0u, 0};
    case Ask::include: {
      IncludeForm form{(q.b & 1) != 0, (q.b & 2) != 0};
      std::string_view spelled = s_->spellings.name(q.a);
      auto r = resolve(s_->files, s_->config, spelled.substr(1, spelled.size() - 2), form,
                       directory_of(s_->files.path(FileId{q.c})), q.d);
      return include_answer(r);
    }
    case Ask::content: {
      const Files::Content* c = s_->files.content(FileId{q.a});
      return c ? c->fp : Fp128{};
    }
    }
    std::unreachable();
  }

  Fp128 include_answer(const std::optional<Resolved>& r) const {
    if (!r) return {};
    return Hasher{}.bytes(s_->files.path(r->file)).uint(r->dir).finish();
  }

  // Applies a reused region's effects: its questions are asked here, its delta and `#pragma
  // once` marks are applied.
  void reuse(const FileRegion& r) {
    for (const Question& q : r.trace) ask(q);
    for (const MacroChange& m : r.delta) {
      if (m.def) macros_.insert_or_assign(m.name, m.def);
      else macros_.erase(m.name);
      touch(m.name);
    }
    for (FileId f : r.once) mark_once(f);
  }

  std::shared_ptr<const FileRegion> finish(Frame& f) {
    FileRegion& r = f.out;
    for (const lex::LexDiagnostic& d : f.lex_diags) r.diags.push_back({0, d.offset, d.kind, d.args});
    std::ranges::stable_sort(r.diags, {}, &Diag::offset);
    std::vector<u32> names(f.touched.begin(), f.touched.end());
    std::ranges::sort(names);
    for (u32 n : names) {
      auto it = macros_.find(n);
      r.delta.push_back({n, it == macros_.end() ? nullptr : it->second});
    }
    r.once.assign(f.once_touched.begin(), f.once_touched.end());
    r.fatal = fatal_;
    return std::make_shared<const FileRegion>(std::move(r));
  }

  // --- questions and state ---------------------------------------------------------------------

  // Records a question in each region being computed, innermost first, until one whose own
  // history decided the answer.
  void ask(const Question& q) {
    for (std::size_t k = frames_.size(); k-- > 0;) {
      Frame& f = *frames_[k];
      switch (q.ask) {
      case Ask::macro:
        if (f.touched.contains(q.a)) return;
        if (!f.asked_macro.insert(q.a).second) continue;
        break;
      case Ask::once:
        if (f.once_touched.contains(FileId{q.a})) return;
        if (!f.asked_once.insert(FileId{q.a}).second) continue;
        break;
      case Ask::content:
        if (!f.asked_content.insert(FileId{q.a}).second) continue;
        break;
      case Ask::include:
        if (std::ranges::contains(f.asked_include, q)) continue;
        f.asked_include.push_back(q);
        break;
      }
      f.out.trace.push_back(q);
    }
  }

  std::shared_ptr<const MacroDef> lookup(u32 name) {
    auto it = macros_.find(name);
    std::shared_ptr<const MacroDef> def = it == macros_.end() ? nullptr : it->second;
    ask(Question{Ask::macro, name, 0, 0, 0, def ? def->fp : Fp128{}});
    return def;
  }

  // Where `d`, the definition of `name`, is as region `k` sees it: in one of its files if its own
  // history defined it, else its binding.
  Bound bound_for(std::size_t k, u32 name, const MacroDef& d) {
    Frame& f = *frames_[k];
    if (f.touched.contains(name)) return Bound{file_index(f, d.file), d.offset, false, 0};
    return Bound{0, 0, true, bind(k, name, d)};
  }
  // Region `k`'s binding of `name`, made when it first uses the entry state's definition.
  u32 bind(std::size_t k, u32 name, const MacroDef& d) {
    Frame& f = *frames_[k];
    if (auto it = f.binding_index.find(name); it != f.binding_index.end()) return it->second;
    u32 b = static_cast<u32>(f.out.bindings.size());
    contract_assert(b < bound_file);
    f.out.bindings.push_back(name);
    f.binding_index.emplace(name, b);
    if (k == 0) f.root_bindings.emplace_back(d.file, d.offset);
    else f.parent_bounds.push_back(bound_for(k - 1, name, d));
    return b;
  }

  // A change of the state by the innermost region is a change by every enclosing one.
  void touch(u32 name) {
    for (auto& f : frames_) f->touched.insert(name);
  }
  void mark_once(FileId file) {
    once_.insert(file);
    for (auto& f : frames_) f->once_touched.insert(file);
  }

  // --- reading tokens --------------------------------------------------------------------------

  void report(Frame& f, u32 offset, lex::LexDiag kind, u64 a = 0, u64 b = 0) {
    f.out.diags.push_back({0, offset, kind, {a, b}});
  }
  void report(Frame& f, const PTok& at, lex::LexDiag kind, u64 a = 0, u64 b = 0) { report(f, at.loc.offset, kind, a, b); }

  // A lexer token as a phase-4 token: spellings moved to the session's table.
  PTok convert(Frame& f, const lex::PpToken& t) {
    PTok p{t.tok, TokLoc{0, 0, f.src->physical(t.begin)}, false};
    switch (t.tok.kind) {
    case Tk::identifier:
    case Tk::header_name: p.tok.payload = s_->spellings.name(f.tables.names[t.tok.payload]); break;
    case Tk::pp_number:
    case Tk::char_lit:
    case Tk::string_lit: {
      Literal l = f.tables.literals[t.tok.payload];
      if (l.ud_suffix != lex::no_name) l.ud_suffix = s_->spellings.name(f.tables.names[l.ud_suffix]);
      p.tok.payload = s_->spellings.literal(std::move(l));
      break;
    }
    default: break;
    }
    return p;
  }

  // The next token of the file, unexpanded and uninterpreted.
  PTok lex_next(Frame& f, bool header_name = false) {
    if (!f.pending.empty()) {
      PTok t = f.pending.front();
      f.pending.pop_front();
      return t;
    }
    return convert(f, header_name ? f.lexer->next_header_name() : f.lexer->next());
  }
  const PTok& peek(Frame& f, std::size_t n, bool header_name = false) {
    while (f.pending.size() <= n) f.pending.push_back(convert(f, header_name ? f.lexer->next_header_name() : f.lexer->next()));
    return f.pending[n];
  }
  static bool same_line(const PTok& t) { return t.tok.kind != Tk::eof && !t.tok.has(tok_flag::line_start); }
  bool is_name(const PTok& t, std::string_view s) const {
    return t.tok.kind == Tk::identifier && s_->spellings.name(t.tok.payload) == s;
  }

  // The rest of a directive's line; the next line's first token stays pending.
  std::vector<PTok> rest_of_line(Frame& f) {
    std::vector<PTok> out;
    for (;;) {
      PTok t = lex_next(f);
      if (!same_line(t)) {
        f.pending.push_front(t);
        return out;
      }
      out.push_back(t);
    }
  }

  // [cpp.pre]/2: `#`, or `import`, `module` or `export` followed on the same line by the right
  // token.
  bool introduces_directive(Frame& f, const PTok& t) {
    if (t.tok.kind == Tk::hash) return true;
    std::size_t at = 0;
    PTok word = t;
    if (is_name(t, "export")) {
      const PTok& n = peek(f, 0);
      if (!same_line(n) || !(is_name(n, "import") || is_name(n, "module"))) return false;
      word = n;
      at = 1;
    }
    if (is_name(word, "import")) {
      const PTok& n = peek(f, at, true);
      return same_line(n) && (n.tok.kind == Tk::header_name || n.tok.kind == Tk::less ||
                              n.tok.kind == Tk::identifier || n.tok.kind == Tk::colon);
    }
    if (is_name(word, "module")) {
      const PTok& n = peek(f, at);
      return same_line(n) && (n.tok.kind == Tk::identifier || n.tok.kind == Tk::colon || n.tok.kind == Tk::semi);
    }
    return false;
  }

  // The next token to rescan: from the innermost context, else from the file, where directives are
  // carried out.
  PTok read(Frame& f) {
    while (!f.contexts.empty()) {
      Context& c = f.contexts.back();
      if (c.pos < c.toks.size()) return with_pending_space(f, c.toks[c.pos++]);
      if (c.barrier) return PTok{Tok{Tk::eof, 0, 0}, {}, false};
      if (c.macro) {
        if (--f.active[*c.macro] == 0) f.active.erase(*c.macro);
      }
      f.contexts.pop_back();
    }
    for (;;) {
      if (fatal_) return PTok{Tok{Tk::eof, tok_flag::line_start, 0}, {}, false};
      PTok t = lex_next(f);
      if (t.tok.kind != Tk::eof && t.tok.has(tok_flag::line_start) && introduces_directive(f, t)) {
        directive(f, t);
        continue;
      }
      return with_pending_space(f, t);
    }
  }
  static PTok with_pending_space(Frame& f, PTok t) {
    if (f.pending_space) {
      t.tok.flags |= tok_flag::space_before;
      f.pending_space = false;
    }
    return t;
  }

  // The next fully replaced token.
  PTok next(Frame& f) {
    for (;;) {
      PTok t = read(f);
      if (t.tok.kind != Tk::identifier || t.painted) return t;
      std::shared_ptr<const MacroDef> def = lookup(t.tok.payload);
      if (!def) return t;
      if (f.active.contains(def->name)) {
        t.painted = true;
        return t;
      }
      if (def->function_like) return t;  // function-like macros: the second part of #85
      replace_object(f, t, *def);
    }
  }

  u16 file_index(Frame& f, FileId file) {
    auto [it, fresh] = f.file_index.emplace(file, u16(f.out.files.size()));
    if (fresh) {
      f.out.files.push_back(file);
      f.out.file_names.push_back(s_->spellings.name(s_->files.path(file)));
    }
    return it->second;
  }

  // [cpp.replace.general]/10: an object-like macro's name is replaced by its replacement list,
  // which is rescanned with the macro disabled.
  void replace_object(Frame& f, const PTok& name, const MacroDef& def) {
    u32 e = static_cast<u32>(f.out.expansions.size());
    // A definition from the entry state is located through a binding, relative to its name.
    Bound where = bound_for(frames_.size() - 1, def.name, def);
    u16 df = where.outer ? u16(bound_file | where.binding) : where.file;
    u32 base = where.outer ? def.offset : 0;
    f.out.expansions.push_back(Expansion{ExpansionKind::object, def.name, name.loc.expansion, name.loc.file,
                                         name.loc.offset, df, def.offset - base, 0, 0});
    Context c;
    c.macro = def.name;
    for (std::size_t i = 0; i < def.body.size(); ++i)
      c.toks.push_back(PTok{def.body[i], TokLoc{df, e, def.offsets[i] - base}, false});
    // The replacement takes the place of the name, and its whitespace ([cpp.rescan]).
    if (!c.toks.empty()) {
      Tok& first = c.toks.front().tok;
      first.flags = u8((first.flags & ~tok_flag::whitespace) | (name.tok.flags & tok_flag::whitespace));
    } else if (name.tok.has(tok_flag::space_before)) {
      f.pending_space = true;
    }
    ++f.active[def.name];
    f.contexts.push_back(std::move(c));
  }

  // --- the file --------------------------------------------------------------------------------

  void process(Frame& f) {
    for (;;) {
      PTok t = next(f);
      if (t.tok.kind == Tk::eof) break;
      f.out.tokens.push_back(t);
    }
  }

  // --- directives ([cpp.pre]) ------------------------------------------------------------------

  void directive(Frame& f, const PTok& intro) {
    if (intro.tok.kind != Tk::hash) {
      // [cpp.module], [cpp.import]: the third part of #85.
      report(f, intro, lex::LexDiag::directive_unsupported);
      rest_of_line(f);
      return;
    }
    PTok d = lex_next(f);
    if (!same_line(d)) {  // the null directive
      f.pending.push_front(d);
      return;
    }
    std::string_view word = d.tok.kind == Tk::identifier ? s_->spellings.name(d.tok.payload) : std::string_view();
    if (word == "include") return include(f, d, IncludeForm{false, false});
    if (word == "include_next") return include(f, d, IncludeForm{false, true});
    if (word == "define") return define(f, d);
    if (word == "undef") return undef(f, d);
    if (word == "pragma") return pragma(f, intro, d);
    if (word == "if" || word == "ifdef" || word == "ifndef" || word == "elif" || word == "elifdef" ||
        word == "elifndef" || word == "else" || word == "endif" || word == "line" || word == "error" ||
        word == "warning" || word == "embed") {
      report(f, d, lex::LexDiag::directive_unsupported);
      rest_of_line(f);
      return;
    }
    report(f, d, lex::LexDiag::invalid_directive);
    rest_of_line(f);
  }

  void extra_tokens(Frame& f, const std::vector<PTok>& rest, std::size_t from) {
    if (from < rest.size()) report(f, rest[from], lex::LexDiag::extra_tokens);
  }

  // The macro name of `#define` and `#undef`, or nothing after reporting why.
  std::optional<PTok> macro_name(Frame& f, const PTok& directive, const std::vector<PTok>& line) {
    if (line.empty()) {
      report(f, directive, lex::LexDiag::macro_name_missing);
      return std::nullopt;
    }
    const PTok& n = line[0];
    if (n.tok.kind != Tk::identifier) {
      report(f, n, lex::LexDiag::macro_name_not_identifier);
      return std::nullopt;
    }
    if (is_name(n, "defined")) {
      report(f, n, lex::LexDiag::macro_name_defined);
      return std::nullopt;
    }
    return n;
  }

  // [cpp.replace.general]
  void define(Frame& f, const PTok& d) {
    std::vector<PTok> line = rest_of_line(f);
    auto name = macro_name(f, d, line);
    if (!name) return;
    if (line.size() > 1 && line[1].tok.kind == Tk::l_paren && !line[1].tok.has(tok_flag::space_before)) {
      report(f, line[1], lex::LexDiag::function_like_unsupported);
      return;
    }
    auto def = std::make_shared<MacroDef>();
    def->name = name->tok.payload;
    def->file = f.file;
    def->offset = name->loc.offset;
    for (std::size_t i = 1; i < line.size(); ++i) {
      if (line[i].tok.kind == Tk::hashhash) {
        report(f, line[i], lex::LexDiag::paste_unsupported);
        return;
      }
      Tok t = line[i].tok;
      t.flags &= u8(~tok_flag::line_start);
      if (i == 1) t.flags &= u8(~tok_flag::space_before);
      def->body.push_back(t);
      def->offsets.push_back(line[i].loc.offset);
    }
    def->fp = fingerprint(*def);
    std::shared_ptr<const MacroDef> old = lookup(def->name);
    if (old && !old->same_as(*def)) report(f, *name, lex::LexDiag::macro_redefined);
    macros_.insert_or_assign(def->name, std::move(def));
    touch(name->tok.payload);
  }

  void undef(Frame& f, const PTok& d) {
    std::vector<PTok> line = rest_of_line(f);
    auto name = macro_name(f, d, line);
    if (!name) return;
    extra_tokens(f, line, 1);
    macros_.erase(name->tok.payload);
    touch(name->tok.payload);
  }

  // The answer of `macro(name)` for a definition: its spelling, whitespace, and its tokens'
  // offsets relative to its name.
  Fp128 fingerprint(const MacroDef& m) const {
    Hasher h;
    h.bytes(s_->spellings.name(m.name)).uint(u8(m.function_like)).uint(u8(m.variadic));
    for (u32 p : m.params) h.bytes(s_->spellings.name(p)).uint(u8(0));
    h.uint(m.body.size());
    for (std::size_t i = 0; i < m.body.size(); ++i) {
      const Tok& t = m.body[i];
      h.uint(std::to_underlying(t.kind)).uint(t.flags).bytes(spelling(t, s_->spellings)).uint(u8(0)).uint(m.offsets[i] - m.offset);
    }
    return h.finish();
  }

  // [cpp.pragma]: `#pragma once` here; other pragmas pass through to `-E`.
  void pragma(Frame& f, const PTok& hash, const PTok& word) {
    std::vector<PTok> line = rest_of_line(f);
    if (!line.empty() && is_name(line[0], "once")) {
      if (frames_.size() == 1)
        report(f, line[0], lex::LexDiag::pragma_once_in_main);
      else
        mark_once(f.file);
      extra_tokens(f, line, 1);
      return;
    }
    Passthrough p{static_cast<u32>(f.out.tokens.size()), {}};
    p.tokens.push_back(hash);
    p.tokens.push_back(word);
    p.tokens.insert(p.tokens.end(), line.begin(), line.end());
    f.out.passthrough.push_back(std::move(p));
  }

  // [cpp.include]
  void include(Frame& f, const PTok& d, IncludeForm form) {
    PTok h = lex_next(f, true);
    std::string spelled;
    std::vector<PTok> line;
    if (h.tok.kind == Tk::header_name && same_line(h)) {
      spelled = s_->spellings.name(h.tok.payload);
      line = rest_of_line(f);
      extra_tokens(f, line, 0);
    } else {
      // [cpp.include]/4: the line is macro-replaced and must form a header name.
      f.pending.push_front(h);
      line = expand_line(f, rest_of_line(f));
      std::size_t used = 0;
      if (!line.empty() && line[0].tok.kind == Tk::string_lit && s_->spellings.literal(line[0].tok.payload).spelling.starts_with('"')) {
        spelled = s_->spellings.literal(line[0].tok.payload).spelling;
        used = 1;
      } else if (!line.empty() && line[0].tok.kind == Tk::less) {
        spelled = "<";
        std::size_t i = 1;
        for (; i < line.size() && line[i].tok.kind != Tk::greater; ++i) {
          if (i > 1 && line[i].tok.has(tok_flag::space_before)) spelled += ' ';
          spelled += spelling(line[i].tok, s_->spellings);
        }
        if (i < line.size()) {
          spelled += '>';
          used = i + 1;
        } else {
          spelled.clear();
        }
      }
      if (spelled.size() < 3) {
        report(f, line.empty() ? d : line[0], lex::LexDiag::include_expected);
        return;
      }
      extra_tokens(f, line, used);
      h = line[0];
    }
    form.angled = spelled.starts_with('<');
    std::string_view header = std::string_view(spelled).substr(1, spelled.size() - 2);
    auto r = resolve(s_->files, s_->config, header, form, f.here, f.found_dir);
    ask(Question{Ask::include, s_->spellings.name(spelled), u32(form.angled) | u32(form.next) << 1,
                 std::to_underlying(f.file), f.found_dir, include_answer(r)});
    if (!r) {
      report(f, h, lex::LexDiag::include_not_found);
      fatal_ = true;
      return;
    }
    bool once = once_.contains(r->file);
    ask(Question{Ask::once, std::to_underlying(r->file), 0, 0, 0, Fp128{once ? 1u : 0u, 0}});
    if (once) return;
    if (frames_.size() >= max_include_depth) {
      report(f, h, lex::LexDiag::include_too_deep);
      fatal_ = true;
      return;
    }
    const Files::Content* c = s_->files.content(r->file);
    ask(Question{Ask::content, std::to_underlying(r->file), 0, 0, 0, c ? c->fp : Fp128{}});
    if (!c) {
      report(f, h, lex::LexDiag::include_not_found);
      fatal_ = true;
      return;
    }
    // The presumed name: as spelled, in the includer's presumed directory or the search directory.
    std::string presumed;
    if (header.starts_with('/')) presumed = header;
    else if (r->dir == includer_dir) {
      std::string dir = directory_of(f.presumed);
      presumed = (dir.empty() ? std::string(".") : dir) + "/" + std::string(header);
    } else {
      u32 nq = static_cast<u32>(s_->config.quote_dirs.size());
      std::string dir = r->dir < nq ? s_->config.quote_dirs[r->dir] : s_->config.angle_dirs[r->dir - nq];
      presumed = dir + (dir.ends_with('/') ? "" : "/") + std::string(header);
    }
    u32 at = static_cast<u32>(f.out.tokens.size());
    std::vector<Bound> bounds;
    std::shared_ptr<const FileRegion> child = enter(r->file, presumed, r->dir, *c, bounds);
    f.out.includes.push_back(Include{at, d.loc.offset, std::move(child), std::move(bounds)});
  }

  // A directive's tokens, fully macro-replaced; replacement does not read past them.
  std::vector<PTok> expand_line(Frame& f, std::vector<PTok> toks) {
    auto saved = std::exchange(f.contexts, {});
    bool space = std::exchange(f.pending_space, false);
    f.contexts.push_back(Context{std::move(toks), 0, std::nullopt, true});
    std::vector<PTok> out;
    for (;;) {
      PTok t = next(f);
      if (t.tok.kind == Tk::eof) break;
      out.push_back(t);
    }
    // Pop the barrier and whatever replacement ended with it.
    while (!f.contexts.empty()) {
      Context& c = f.contexts.back();
      if (c.macro) {
        if (--f.active[*c.macro] == 0) f.active.erase(*c.macro);
      }
      f.contexts.pop_back();
    }
    f.contexts = std::move(saved);
    f.pending_space = space;
    return out;
  }

  Session* s_;
  std::vector<std::unique_ptr<Frame>> frames_;
  std::unordered_map<u32, std::shared_ptr<const MacroDef>> macros_;
  std::set<FileId> once_;
  bool fatal_ = false;
};

}  // namespace ycxx::pp
