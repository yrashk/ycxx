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
import :predefined;

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
    bool root = false;  // a main file: `#pragma once` there is diagnosed, not applied
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
    std::string s = predefined_text(*config.target);
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
  std::vector<RootBinding> builtins_bindings;
  std::vector<RootBinding> main_bindings;
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
    bool space_after = false;  // the token after it follows whitespace (a trailing placemarker's)
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
    u32 collecting = 0;  // invocations whose arguments are being collected
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
    std::vector<RootBinding> root_bindings;
  };

  // --- regions ---------------------------------------------------------------------------------

  std::shared_ptr<const FileRegion> enter_root(FileId file, std::string_view presumed, std::vector<RootBinding>& bindings) {
    const Files::Content* c = s_->files.content(file);
    if (!c) return nullptr;
    std::vector<Bound> ignored;
    return enter(file, presumed, includer_dir, *c, ignored, &bindings);
  }

  // The region of `file` here: a cached one whose trace verifies, or a new one. `bounds` gets its
  // bindings as the current region sees them, or `root` as the TU does.
  std::shared_ptr<const FileRegion> enter(FileId file, std::string_view presumed, u32 found_dir, const Files::Content& c,
                                          std::vector<Bound>& bounds, std::vector<RootBinding>* root = nullptr) {
    Session::Key key{file, s_->spellings.name(presumed), found_dir, root != nullptr, c.fp};
    if (auto it = s_->cache.find(key); it != s_->cache.end()) {
      for (const std::shared_ptr<const FileRegion>& r : it->second) {
        // The nesting limit is part of the value: a region reused deeper must still fit.
        if (frames_.size() + r->depth <= max_include_depth && verifies(*r)) {
          // Its bindings are the entry state's definitions.
          for (u32 name : r->bindings) {
            const MacroDef& d = *macros_.at(name);
            if (root) root->push_back({d.file, d.file_name, d.offset});
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
    for (const Include& i : r.includes)
      if (i.region) r.depth = std::max(r.depth, i.region->depth + 1);
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
    if (f.touched.contains(name)) return Bound{file_index(f, d.file, d.file_name), d.offset, false, 0};
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
    if (k == 0) f.root_bindings.push_back({d.file, d.file_name, d.offset});
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
  // At a token's site in the file: a replaced token is reported where its expansion began, but a
  // token written in a macro's argument where it is written.
  void report(Frame& f, const PTok& at, lex::LexDiag kind, u64 a = 0, u64 b = 0) {
    TokLoc l = at.loc;
    while (l.expansion != 0) {
      const Expansion& e = f.out.expansions[l.expansion];
      if (e.kind == ExpansionKind::argument && l.file == 0) return report(f, l.offset, kind, a, b);
      l = TokLoc{e.site_file, e.parent, e.site_offset};
    }
    report(f, l.offset, kind, a, b);
  }
  static u32 site(const Frame& f, const PTok& t) {
    TokLoc l = t.loc;
    while (l.expansion != 0) {
      const Expansion& e = f.out.expansions[l.expansion];
      l = TokLoc{e.site_file, e.parent, e.site_offset};
    }
    return l.offset;
  }

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
  // `base` is how many tokens are pending before the ones after `t`.
  bool introduces_directive(Frame& f, const PTok& t, std::size_t base = 0) {
    if (t.tok.kind == Tk::hash) return true;
    std::size_t at = base;
    PTok word = t;
    if (is_name(t, "export")) {
      const PTok& n = peek(f, base);
      if (!same_line(n) || !(is_name(n, "import") || is_name(n, "module"))) return false;
      word = n;
      at = base + 1;
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
      if (c.space_after) f.pending_space = true;
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
  // Whether the next token read comes from the file and starts a directive.
  bool directive_next(Frame& f) {
    for (auto it = f.contexts.rbegin(); it != f.contexts.rend(); ++it) {
      if (it->pos < it->toks.size() || it->barrier) return false;
    }
    if (fatal_) return false;
    PTok t = peek(f, 0);
    return t.tok.kind != Tk::eof && t.tok.has(tok_flag::line_start) && introduces_directive(f, t, 1);
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
      std::vector<Arg> args;
      if (def->function_like) {
        PTok close;
        bool drop = false;
        auto a = invocation(f, t, *def, close, drop);
        if (!a) {
          if (drop) continue;
          return t;
        }
        args = std::move(*a);
        replace(f, t, *def, args, site(f, close) + 1);
        continue;
      }
      replace(f, t, *def, args, 0);
    }
  }

  u16 file_index(Frame& f, FileId file, u32 name) {
    auto [it, fresh] = f.file_index.emplace(file, u16(f.out.files.size()));
    if (fresh) {
      f.out.files.push_back(file);
      f.out.file_names.push_back(name);
    }
    return it->second;
  }

  // --- replacement ([cpp.replace], [cpp.subst], [cpp.stringize], [cpp.concat]) ------------------

  // An argument as written, its tokens unreplaced.
  struct Arg {
    std::vector<PTok> raw;
    // Its replacement, made once ([cpp.subst]/1) at its first use, whose argument expansion is
    // `tag`; later uses relocate it.
    std::optional<std::vector<PTok>> replaced;
    u32 tag = 0;
  };
  // A token of a replacement being built: a placemarker stands for an empty argument
  // ([cpp.concat]/2); `paste` marks a `##` operator of the replacement list.
  struct Item {
    PTok tok;
    bool placemarker = false;
    bool paste = false;
  };
  // Where the replacement list of a macro being replaced is: its file (or binding) and the base
  // its offsets are relative to.
  struct Where {
    u16 file = 0;
    u32 base = 0;
    u32 expansion = 0;
  };

  // [cpp.replace.general]/11: a function-like macro's name followed by `(` starts an invocation,
  // whose arguments run to the matching `)`; new-lines in it are whitespace. Nothing if the next
  // token is not `(` (it is read again), or if the invocation is ill-formed (reported; `drop`
  // when the name goes too, as it does when the invocation is unterminated). A directive ends
  // the search for `(`, as it ends a text line.
  std::optional<std::vector<Arg>> invocation(Frame& f, const PTok& name, const MacroDef& def, PTok& close, bool& drop) {
    if (directive_next(f)) return std::nullopt;
    PTok n = read(f);
    if (n.tok.kind != Tk::l_paren) {
      f.contexts.push_back(Context{{n}, 0, std::nullopt, false});
      return std::nullopt;
    }
    std::vector<Arg> args(1);
    std::size_t depth = 0;
    const std::size_t named = def.params.size();
    ++f.collecting;
    struct Done {
      Frame& f;
      ~Done() { --f.collecting; }
    } done{f};
    for (;;) {
      PTok a = read(f);
      if (a.tok.kind == Tk::eof) {
        report(f, name, lex::LexDiag::invocation_unterminated);
        f.contexts.push_back(Context{{a}, 0, std::nullopt, false});
        drop = true;
        return std::nullopt;
      }
      if (a.tok.kind == Tk::l_paren) ++depth;
      else if (a.tok.kind == Tk::r_paren && depth-- == 0) {
        close = a;
        break;
      } else if (a.tok.kind == Tk::comma && depth == 0 && !(def.variadic && args.size() == named + 1)) {
        args.emplace_back();
        continue;
      }
      if (a.tok.has(tok_flag::line_start)) a.tok.flags = u8((a.tok.flags & ~tok_flag::line_start) | tok_flag::space_before);
      // [cpp.rescan]/2: a name read while its macro is disabled stays painted, even after the
      // macro is enabled again.
      if (a.tok.kind == Tk::identifier && f.active.contains(a.tok.payload)) a.painted = true;
      args.back().raw.push_back(a);
    }
    // [cpp.replace.general]/4: as many arguments as parameters; a variadic macro's variable
    // arguments may be omitted ([cpp.replace.general]/12).
    const std::size_t k = args.size();
    if (named == 0 && !def.variadic && k == 1 && args[0].raw.empty()) args.clear();
    // Ill-formed or not, the invocation ends at its `)`.
    f.out.continuations.push_back({static_cast<u32>(f.out.tokens.size()), site(f, name), site(f, close)});
    if (k < named) {
      report(f, close, lex::LexDiag::too_few_arguments);
      return std::nullopt;
    }
    if (def.variadic && k == named) args.emplace_back();  // the variable arguments omitted
    if (args.size() > named + (def.variadic ? 1 : 0)) {
      const std::vector<PTok>& first = args[std::min(named, args.size() - 1)].raw;
      report(f, first.empty() ? close : first.front(), lex::LexDiag::too_many_arguments);
      return std::nullopt;
    }
    return args;
  }

  // A macro's name is replaced by its replacement list, arguments substituted, and rescanned with
  // the macro disabled ([cpp.replace.general]/10–11, [cpp.rescan]).
  // `end` is 1 + the offset of the invocation's `)` in the file, which `-E` continues the line
  // after (0 for an object-like macro).
  void replace(Frame& f, const PTok& name, const MacroDef& def, std::span<Arg> args, u32 end) {
    Where w;
    w.expansion = static_cast<u32>(f.out.expansions.size());
    // A definition from the entry state is located through a binding, relative to its name.
    Bound where = bound_for(frames_.size() - 1, def.name, def);
    w.file = where.outer ? u16(bound_file | where.binding) : where.file;
    w.base = where.outer ? def.offset : 0;
    f.out.expansions.push_back(Expansion{def.function_like ? ExpansionKind::function : ExpansionKind::object, def.name,
                                         name.loc.expansion, name.loc.file, name.loc.offset, w.file, def.offset - w.base, end, 0});
    // [cpp.subst]/3: __VA_OPT__ is replaced when the variable arguments, replaced, are not empty.
    bool va = false;
    for (std::size_t i = 0; i < def.body.size(); ++i) {
      if (is_va_opt(def, def.body[i])) {
        va = !argument(f, def, w, args, def.params.size(), i, false).empty();
        break;
      }
    }
    std::vector<Item> items = substitute(f, def, w, args, 0, def.body.size(), va);
    Context c;
    c.macro = def.name;
    // A placemarker's whitespace goes to the token after it, as an empty replacement's does.
    bool space = false;
    for (const Item& i : items) {
      if (i.placemarker) {
        space = space || i.tok.tok.has(tok_flag::space_before);
        continue;
      }
      c.toks.push_back(i.tok);
      if (std::exchange(space, false)) c.toks.back().tok.flags |= tok_flag::space_before;
    }
    // The replacement takes the place of the name, and its whitespace.
    if (!c.toks.empty()) {
      Tok& first = c.toks.front().tok;
      first.flags = u8((first.flags & ~tok_flag::whitespace) | (name.tok.flags & tok_flag::whitespace));
    } else if (name.tok.has(tok_flag::space_before)) {
      f.pending_space = true;
    }
    c.space_after = space && !c.toks.empty();
    ++f.active[def.name];
    f.contexts.push_back(std::move(c));
  }

  TokLoc body_loc(const MacroDef& def, const Where& w, std::size_t i, u32 expansion) const {
    return TokLoc{w.file, expansion, def.offsets[i] - w.base};
  }

  // The parameter a replacement-list token names: its index, `params.size()` for `__VA_ARGS__`.
  std::optional<std::size_t> param(const MacroDef& def, const Tok& t) const {
    if (!def.function_like || t.kind != Tk::identifier) return std::nullopt;
    for (std::size_t k = 0; k < def.params.size(); ++k)
      if (def.params[k] == t.payload) return k;
    if (def.variadic && t.payload == va_args_) return def.params.size();
    return std::nullopt;
  }
  bool is_va_opt(const MacroDef& def, const Tok& t) const {
    return def.variadic && t.kind == Tk::identifier && t.payload == va_opt_;
  }
  // The `)` closing `__VA_OPT__(` at `i` (checked balanced at definition).
  static std::size_t va_opt_end(const MacroDef& def, std::size_t i) {
    std::size_t depth = 0;
    for (std::size_t j = i + 1; j < def.body.size(); ++j) {
      if (def.body[j].kind == Tk::l_paren) ++depth;
      else if (def.body[j].kind == Tk::r_paren && --depth == 0) return j;
    }
    std::unreachable();
  }

  // Argument `p` for its use at replacement-list token `at`: its tokens in an expansion of kind
  // argument, fully replaced unless `raw` ([cpp.subst]/1).
  std::vector<PTok> argument(Frame& f, const MacroDef& def, const Where& w, std::span<Arg> args, std::size_t p,
                             std::size_t at, bool raw) {
    u32 ae = static_cast<u32>(f.out.expansions.size());
    TokLoc site = body_loc(def, w, at, w.expansion);
    f.out.expansions.push_back(Expansion{ExpansionKind::argument, def.name, w.expansion, site.file, site.offset, w.file,
                                         def.offset - w.base, static_cast<u32>(p), 0});
    Arg& a = args[p];
    if (raw) {
      std::vector<PTok> toks = a.raw;
      for (PTok& t : toks) t.loc.expansion = ae;
      return toks;
    }
    if (!a.replaced) {
      std::vector<PTok> toks = a.raw;
      for (PTok& t : toks) t.loc.expansion = ae;
      a.replaced = expand_line(f, std::move(toks));
      a.tag = ae;
      return *a.replaced;
    }
    std::vector<PTok> toks = *a.replaced;
    for (PTok& t : toks)
      if (t.loc.expansion == a.tag) t.loc.expansion = ae;
    return toks;
  }

  // [cpp.subst], [cpp.stringize], [cpp.concat] over replacement-list tokens [b, end).
  std::vector<Item> substitute(Frame& f, const MacroDef& def, const Where& w, std::span<Arg> args, std::size_t b,
                               std::size_t end, bool va) {
    std::vector<Item> out;
    const std::vector<Tok>& body = def.body;
    auto is_paste = [&](std::size_t j) { return j >= b && j < end && body[j].kind == Tk::hashhash; };
    for (std::size_t i = b; i < end; ++i) {
      const Tok& t = body[i];
      const u8 ws = t.flags & tok_flag::whitespace;
      if (def.function_like && t.kind == Tk::hash && i + 1 < end) {
        const Tok& n = body[i + 1];
        std::string text;
        if (is_va_opt(def, n)) {
          std::size_t j = va_opt_end(def, i + 1);
          std::vector<PTok> toks;
          if (va)
            for (const Item& it : substitute(f, def, w, args, i + 3, j, va))
              if (!it.placemarker) toks.push_back(it.tok);
          text = stringize(toks);
          i = j;
        } else {
          std::size_t p = *param(def, n);
          text = stringize(args[p].raw);
          i += 1;
        }
        PTok s = string_token(f, text, body_loc(def, w, i, w.expansion));
        s.tok.flags = ws;
        s.loc = body_loc(def, w, i, w.expansion);
        out.push_back({s});
        continue;
      }
      if (is_va_opt(def, t)) {
        std::size_t j = va_opt_end(def, i);
        std::vector<Item> content;
        if (va) content = substitute(f, def, w, args, i + 2, j, va);
        if (content.empty()) content.push_back(Item{PTok{Tok{}, body_loc(def, w, i, w.expansion), false}, true, false});
        content.front().tok.tok.flags = u8((content.front().tok.tok.flags & ~tok_flag::whitespace) | ws);
        out.insert(out.end(), content.begin(), content.end());
        i = j;
        continue;
      }
      if (auto p = param(def, t)) {
        bool raw = is_paste(i - 1) || is_paste(i + 1);
        std::vector<PTok> toks = argument(f, def, w, args, *p, i, raw);
        if (toks.empty()) {
          out.push_back(Item{PTok{Tok{Tk::eof, ws, 0}, body_loc(def, w, i, w.expansion), false}, true, false});
          continue;
        }
        toks.front().tok.flags = u8((toks.front().tok.flags & ~tok_flag::whitespace) | ws);
        for (PTok& a : toks) out.push_back({a});
        continue;
      }
      out.push_back(Item{PTok{t, body_loc(def, w, i, w.expansion), false}, false, t.kind == Tk::hashhash});
    }
    return paste(f, std::move(out));
  }

  // [cpp.concat]/3: each `##` is deleted and the tokens around it concatenated; placemarkers
  // vanish into their neighbours.
  std::vector<Item> paste(Frame& f, std::vector<Item> items) {
    std::vector<Item> out;
    for (std::size_t k = 0; k < items.size(); ++k) {
      if (!items[k].paste || out.empty() || k + 1 == items.size()) {
        out.push_back(items[k]);
        continue;
      }
      const PTok op = items[k].tok;
      Item rhs = items[++k];
      Item& lhs = out.back();
      if (rhs.placemarker) continue;
      if (lhs.placemarker) {
        u8 ws = lhs.tok.tok.flags & tok_flag::whitespace;
        lhs = rhs;
        lhs.tok.tok.flags = u8((lhs.tok.tok.flags & ~tok_flag::whitespace) | ws);
        lhs.paste = false;
        continue;
      }
      std::string text = spelling(lhs.tok.tok, s_->spellings) + spelling(rhs.tok.tok, s_->spellings);
      auto r = lex_one(text);
      if (!r) {
        report(f, op, lex::LexDiag::paste_invalid);
        rhs.paste = false;
        out.push_back(rhs);
        continue;
      }
      // ast.md §8.6: spelled where the first operand is, in an expansion of kind paste.
      u32 pe = static_cast<u32>(f.out.expansions.size());
      f.out.expansions.push_back(Expansion{ExpansionKind::paste, 0, lhs.tok.loc.expansion, lhs.tok.loc.file,
                                           lhs.tok.loc.offset, rhs.tok.loc.file, rhs.tok.loc.offset, 0, 0});
      r->tok.flags = u8((r->tok.flags & ~tok_flag::whitespace) | (lhs.tok.tok.flags & tok_flag::whitespace));
      r->loc = TokLoc{lhs.tok.loc.file, pe, lhs.tok.loc.offset};
      lhs.tok = *r;
    }
    return out;
  }

  // [cpp.stringize]/2: the spellings, one space where there was whitespace, `"` and `\` escaped
  // inside string and character literals.
  std::string stringize(std::span<const PTok> toks) const {
    std::string s = "\"";
    for (std::size_t k = 0; k < toks.size(); ++k) {
      const Tok& t = toks[k].tok;
      if (k > 0 && t.has(tok_flag::whitespace)) s += ' ';
      std::string text = spelling(t, s_->spellings);
      if (t.kind == Tk::string_lit || t.kind == Tk::char_lit) {
        for (char c : text) {
          if (c == '\n') {  // in a raw string literal written across lines
            s += "\\n";
            continue;
          }
          if (c == '"' || c == '\\') s += '\\';
          s += c;
        }
      } else {
        s += text;
      }
    }
    return s + '"';
  }

  // [cpp.stringize]/2: the string literal stringizing made; one that is not valid is diagnosed and
  // kept as it is spelled, without a value.
  PTok string_token(Frame& f, const std::string& text, TokLoc at) {
    if (auto s = lex_one(text)) {
      s->loc = at;
      return *s;
    }
    report(f, PTok{Tok{}, at, false}, lex::LexDiag::stringize_invalid);
    Literal l;
    l.spelling = text;
    l.bad = true;
    return PTok{Tok{Tk::string_lit, 0, s_->spellings.literal(std::move(l))}, at, false};
  }

  // The one preprocessing token `text` spells, or nothing if it spells none or several.
  std::optional<PTok> lex_one(std::string_view text) {
    std::vector<lex::LexDiagnostic> diags;
    lex::Source src(text, diags);
    lex::Tables tables;
    lex::Lexer lx(src, tables, diags);
    lex::PpToken t = lx.next();
    if (t.tok.kind == Tk::eof || t.begin != 0 || t.end != text.size() || lx.next().tok.kind != Tk::eof || !diags.empty())
      return std::nullopt;
    PTok p{t.tok, {}, false};
    p.tok.flags &= tok_flag::alt;
    switch (t.tok.kind) {
    case Tk::identifier: p.tok.payload = s_->spellings.name(tables.names[t.tok.payload]); break;
    case Tk::pp_number:
    case Tk::char_lit:
    case Tk::string_lit: {
      Literal l = tables.literals[t.tok.payload];
      if (l.ud_suffix != lex::no_name) l.ud_suffix = s_->spellings.name(tables.names[l.ud_suffix]);
      p.tok.payload = s_->spellings.literal(std::move(l));
      break;
    }
    default: break;
    }
    return p;
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
    auto def = std::make_shared<MacroDef>();
    def->name = name->tok.payload;
    def->file = f.file;
    def->file_name = f.out.presumed;
    def->offset = name->loc.offset;
    std::size_t i = 1;
    // A `(` right after the name starts the parameters of a function-like macro.
    if (i < line.size() && line[i].tok.kind == Tk::l_paren && !line[i].tok.has(tok_flag::space_before)) {
      def->function_like = true;
      ++i;
      for (bool first = true;; first = false) {
        if (i < line.size() && line[i].tok.kind == Tk::r_paren && first) break;
        if (i < line.size() && line[i].tok.kind == Tk::ellipsis) {
          def->variadic = true;
          ++i;
        } else if (i < line.size() && line[i].tok.kind == Tk::identifier && line[i].tok.payload != va_args_ &&
                   line[i].tok.payload != va_opt_) {
          if (std::ranges::contains(def->params, line[i].tok.payload)) {
            report(f, line[i], lex::LexDiag::macro_param_duplicate);
            return;
          }
          def->params.push_back(line[i++].tok.payload);
        } else {
          report(f, i < line.size() ? line[i] : line.back(), lex::LexDiag::macro_param_expected);
          return;
        }
        if (i < line.size() && line[i].tok.kind == Tk::r_paren) break;
        if (def->variadic || i >= line.size() || line[i].tok.kind != Tk::comma) {
          report(f, i < line.size() ? line[i] : line.back(), lex::LexDiag::macro_param_expected);
          return;
        }
        ++i;
      }
      ++i;
    }
    for (std::size_t k = i; k < line.size(); ++k) {
      Tok t = line[k].tok;
      t.flags &= u8(~tok_flag::line_start);
      if (k == i) t.flags &= u8(~tok_flag::space_before);
      def->body.push_back(t);
      def->offsets.push_back(line[k].loc.offset);
    }
    if (auto bad = check_body(*def)) {
      // At the offending token, or just past the line's last.
      std::size_t at = i + bad->first;
      if (at < line.size()) report(f, line[at], bad->second);
      else report(f, site(f, line.back()) + static_cast<u32>(spelling(line.back().tok, s_->spellings).size()), bad->second);
      return;
    }
    def->fp = fingerprint(*def);
    std::shared_ptr<const MacroDef> old = lookup(def->name);
    if (old && !old->same_as(*def)) report(f, *name, lex::LexDiag::macro_redefined);
    macros_.insert_or_assign(def->name, std::move(def));
    touch(name->tok.payload);
  }

  // The constraints on a replacement list: the first violated, at its token.
  std::optional<std::pair<std::size_t, lex::LexDiag>> check_body(const MacroDef& def) const {
    const std::vector<Tok>& b = def.body;
    // [cpp.concat]/1: not at either end, nor at either end of a __VA_OPT__'s replacement.
    if (!b.empty() && b.front().kind == Tk::hashhash) return std::pair{std::size_t(0), lex::LexDiag::paste_at_edge};
    if (!b.empty() && b.back().kind == Tk::hashhash) return std::pair{b.size() - 1, lex::LexDiag::paste_at_edge};
    for (std::size_t k = 0; k < b.size(); ++k) {
      const Tok& t = b[k];
      // [cpp.replace.general]/5–6: __VA_ARGS__ and __VA_OPT__ only in a variadic macro.
      if (t.kind == Tk::identifier && (t.payload == va_args_ || t.payload == va_opt_) && !def.variadic)
        return std::pair{k, lex::LexDiag::va_outside_variadic};
      if (is_va_opt(def, t)) {
        // [cpp.subst]/3: `__VA_OPT__ ( pp-tokens )`, not nested.
        if (k + 1 >= b.size() || b[k + 1].kind != Tk::l_paren) return std::pair{k + 1, lex::LexDiag::va_opt_malformed};
        std::size_t depth = 0, j = k + 1;
        for (; j < b.size(); ++j) {
          if (b[j].kind == Tk::l_paren) ++depth;
          else if (b[j].kind == Tk::r_paren && --depth == 0) break;
          else if (is_va_opt(def, b[j])) return std::pair{j, lex::LexDiag::va_opt_malformed};
        }
        if (j >= b.size()) return std::pair{k, lex::LexDiag::va_opt_malformed};
        if (j > k + 2 && (b[k + 2].kind == Tk::hashhash || b[j - 1].kind == Tk::hashhash))
          return std::pair{b[k + 2].kind == Tk::hashhash ? k + 2 : j, lex::LexDiag::paste_at_edge};  // a trailing one at `)`
      }
      // [cpp.stringize]/1: in a function-like macro, `#` is followed by a parameter.
      if (def.function_like && t.kind == Tk::hash &&
          (k + 1 >= b.size() || (!param(def, b[k + 1]) && !is_va_opt(def, b[k + 1]))))
        return std::pair{k + 1, lex::LexDiag::stringize_not_parameter};
    }
    return std::nullopt;
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
    if (f.collecting > 0) {  // as for #include: Clang rejects it too
      report(f, word, lex::LexDiag::directive_in_arguments);
      return;
    }
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
    p.tokens.back().tok.flags &= u8(~tok_flag::space_before);  // `#pragma`, as Clang prints it
    p.tokens.insert(p.tokens.end(), line.begin(), line.end());
    f.out.passthrough.push_back(std::move(p));
  }

  // [cpp.include]
  void include(Frame& f, const PTok& d, IncludeForm form) {
    if (f.collecting > 0) {  // [cpp.replace.general]/13 leaves it undefined; Clang rejects it too
      report(f, d, lex::LexDiag::directive_in_arguments);
      rest_of_line(f);
      return;
    }
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
    // Placed after the directive's own diagnostics, at its last token.
    u32 end = line.empty() ? site(f, h) : site(f, line.back());
    f.out.includes.push_back(Include{at, end, std::move(child), std::move(bounds)});
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
  u32 va_args_ = s_->spellings.name("__VA_ARGS__");
  u32 va_opt_ = s_->spellings.name("__VA_OPT__");
  std::vector<std::unique_ptr<Frame>> frames_;
  std::unordered_map<u32, std::shared_ptr<const MacroDef>> macros_;
  std::set<FileId> once_;
  bool fatal_ = false;
};

}  // namespace ycxx::pp
