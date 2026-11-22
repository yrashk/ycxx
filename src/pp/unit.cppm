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
import ycxx.ast.target;
import ycxx.ast.typeops;
import ycxx.ast.types;
import ycxx.hash;
import ycxx.lex;
import :spell;
import :files;
import :region;
import :predefined;
import :cond;

export namespace ycxx::pp {

// What preprocessing shares across translation units: spellings, files, the configuration and
// the regions computed so far.
class Session {
public:
  explicit Session(Host& host, Config cfg = {})
      : files(host), build_time(now()), config(std::move(cfg)), config_fp(config.fingerprint()) {}
  Session(const Session&) = delete;

  // Held by whoever reads or extends the session (`Pp`, rendering a diagnostic, `-E`), so engine
  // workers can share it (#137). `Pp` runs without suspending, so it is never held across a wait.
  std::mutex mutex;
  Spellings spellings;
  Files files;
  // `__DATE__` and `__TIME__` ([cpp.predefined]): the session's start, the same for all its TUs.
  const std::pair<std::string, std::string> build_time;
  static std::pair<std::string, std::string> now() {
    auto t = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto days = std::chrono::floor<std::chrono::days>(t);
    std::chrono::year_month_day ymd{days};
    std::chrono::hh_mm_ss hms{t - days};
    static constexpr std::array<std::string_view, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return {std::format("\"{} {:>2} {}\"", months[unsigned(ymd.month()) - 1], unsigned(ymd.day()), int(ymd.year())),
            std::format("\"{:02}:{:02}:{:02}\"", hms.hours().count(), hms.minutes().count(), hms.seconds().count())};
  }
  const Config config;
  const Fp128 config_fp;

  // Regions by file occurrence, each entry valid where its trace verifies.
  struct Key {
    FileId file{};
    u32 presumed = 0;
    u32 found_dir = 0;
    bool root = false;  // a main file: `#pragma once` there is diagnosed, not applied
    Fp128 content;
    bool system = false;  // a system header: its warnings are dropped
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
  explicit Unit(Session& s) : s_(&s) {
    using enum MacroDef::Dynamic;
    for (auto [name, kind] : {std::pair{"__LINE__", line}, {"__FILE__", file}, {"__COUNTER__", counter},
                              {"__DATE__", date}, {"__TIME__", time}}) {
      auto d = std::make_shared<MacroDef>();
      d->name = s_->spellings.name(name);
      d->dynamic = kind;
      d->file = s_->files.id("<built-in>");
      d->file_name = s_->spellings.name("<built-in>");
      d->fp = Hasher{}.bytes("dynamic").bytes(name).finish();
      macros_.emplace(d->name, std::move(d));
    }
  }

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
    bool system = false;  // a system header (Config::system_header); the main file never is
    std::string here;     // the directory of the real path
    std::string presumed;
    std::string_view text;  // the file's physical text
    std::vector<u32> line_starts;
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
    std::unordered_set<u32> asked_defined;
    bool asked_counter = false, counter_touched = false, asked_time = false;
    u32 counter_entry = 0;
    // [cpp.line]: the presumed line is the physical one plus `line_delta`; `presumed_name` replaces
    // the presumed name.
    std::int32_t line_delta = 0;
    std::optional<std::string> presumed_name;
    // Conditional inclusion ([cpp.cond]): the open groups, and whether a #if's expression is being
    // replaced (where `defined` and the `__has_*` operators apply).
    struct Cond {
      bool taken = false;  // a group of this #if was included
      bool in_else = false;
      u32 offset = 0;      // of the #if
    };
    std::vector<Cond> conds;
    bool in_condition = false;
    bool in_embed_probe = false;
    u32 replacing_argument = 0;  // arguments being replaced ([cpp.subst]/1)
    // The include guard: 0 nothing yet; 1 inside `#ifndef guard`'s group; 2 after its #endif; -1
    // none.
    int guard_stage = 0;
    u32 guard = 0;
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
    return enter(file, presumed, includer_dir, false, *c, ignored, &bindings);
  }

  // The region of `file` here: a cached one whose trace verifies, or a new one. `bounds` gets its
  // bindings as the current region sees them, or `root` as the TU does.
  std::shared_ptr<const FileRegion> enter(FileId file, std::string_view presumed, u32 found_dir, bool system,
                                          const Files::Content& c, std::vector<Bound>& bounds,
                                          std::vector<RootBinding>* root = nullptr) {
    Session::Key key{file, s_->spellings.name(presumed), found_dir, root != nullptr, c.fp, system};
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
      // §B9: a guarded file included where its guard is defined is empty. That is a region too,
      // whose one question is the guard's, so it is not read again.
      for (const std::shared_ptr<const FileRegion>& r : it->second) {
        // Not when skipping it again would say something (a diagnostic, a pragma).
        if (!r->guard || !macros_.contains(*r->guard) || !r->diags.empty() || !r->passthrough.empty()) continue;
        auto empty = std::make_shared<FileRegion>();
        empty->file = file;
        empty->presumed = key.presumed;
        empty->content = c.fp;
        empty->files = {file};
        empty->file_names = {key.presumed};
        empty->trace = {Question{Ask::defined, *r->guard, 0, 0, 0, Fp128{1, 0}}};
        empty->guard = r->guard;
        empty->system = system;
        it->second.push_back(empty);
        reuse(*empty);
        ++s_->stats.reused;
        return empty;
      }
    }
    auto f = std::make_unique<Frame>();
    f->file = file;
    f->found_dir = found_dir;
    f->system = system;
    f->here = directory_of(s_->files.path(file));
    f->presumed = presumed;
    f->text = c.text;
    f->counter_entry = counter_;
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
    case Ask::defined: return Fp128{macros_.contains(q.a) ? 1u : 0u, 0};
    case Ask::counter: return Fp128{counter_, 0};
    case Ask::time: return time_answer();
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
    if (r.counter > 0) {
      counter_ += r.counter;
      for (auto& fr : frames_) fr->counter_touched = true;
    }
  }

  std::shared_ptr<const FileRegion> finish(Frame& f) {
    FileRegion& r = f.out;
    for (const lex::LexDiagnostic& d : f.lex_diags) r.diags.push_back({0, d.offset, d.kind, d.args, 0, std::nullopt});
    // A system header's warnings are not reported (`hidden_in_system_header`); phases 5 and 6
    // drop theirs when flattened (:tu).
    r.system = f.system;
    if (f.system) std::erase_if(r.diags, [](const Diag& d) { return hidden_in_system_header(d.kind); });
    std::ranges::stable_sort(r.diags, {}, &Diag::offset);
    // The presumed locations of this occurrence ([cpp.line]).
    if (!r.lines.empty()) {
      for (Diag& d : r.diags) {
        auto l = std::ranges::upper_bound(r.lines, d.offset, {}, &FileRegion::Line::at);
        if (l == r.lines.begin()) continue;
        --l;
        d.presumed_line = static_cast<u32>(static_cast<std::int64_t>(physical_line(f, d.offset)) + l->delta);
        d.presumed_name = l->name;
      }
    }
    std::vector<u32> names(f.touched.begin(), f.touched.end());
    std::ranges::sort(names);
    for (u32 n : names) {
      auto it = macros_.find(n);
      r.delta.push_back({n, it == macros_.end() ? nullptr : it->second});
    }
    r.once.assign(f.once_touched.begin(), f.once_touched.end());
    r.counter = counter_ - f.counter_entry;
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
      case Ask::defined:
        if (f.touched.contains(q.a)) return;
        if (!f.asked_defined.insert(q.a).second) continue;
        break;
      case Ask::counter:
        if (f.counter_touched) return;
        if (std::exchange(f.asked_counter, true)) continue;
        break;
      case Ask::time:
        if (std::exchange(f.asked_time, true)) continue;
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
    f.out.diags.push_back({0, offset, kind, {a, b}, 0, std::nullopt});
  }
  // At a token's site in the file: a replaced token is reported where its expansion began, but a
  // token written in a macro's argument where it is written.
  void report(Frame& f, const PTok& at, lex::LexDiag kind, u64 a = 0, u64 b = 0) { report(f, written(f, at), kind, a, b); }
  // Where a token is in the file: its site, but for a token written in a macro's argument where
  // it is written.
  static u32 written(const Frame& f, const PTok& t) {
    TokLoc l = t.loc;
    while (l.expansion != 0) {
      const Expansion& e = f.out.expansions[l.expansion];
      if (e.kind == ExpansionKind::argument && l.file == 0) return l.offset;
      l = TokLoc{e.site_file, e.parent, e.site_offset};
    }
    return l.offset;
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
  std::vector<PTok> rest_of_line(Frame& f, bool header_probes = false) {
    std::vector<PTok> out;
    for (;;) {
      const bool after_probe = header_probes && out.size() >= 2 && out.back().tok.kind == Tk::l_paren &&
                               (is_name(out[out.size() - 2], "__has_embed") ||
                                is_name(out[out.size() - 2], "__has_include"));
      PTok t = lex_next(f, after_probe);
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

  // The identifiers protected by [cpp.pre] and [cpp.cond] during embed replacement.
  static bool embed_macro_name(std::string_view name) {
    return name == "limit" || name == "prefix" || name == "suffix" || name == "if_empty";
  }

  // The next fully replaced token.
  PTok next(Frame& f) {
    for (;;) {
      PTok t = read(f);
      if (t.tok.kind != Tk::identifier) return t;
      std::string_view name = s_->spellings.name(t.tok.payload);
      if (f.in_embed_probe && embed_macro_name(name) && is_defined(t.tok.payload)) {
        report(f, t, lex::LexDiag::embed_parameter_macro);
        return t;
      }
      if (t.painted) return t;
      if (f.in_condition && (name == "defined" || condition_operator(t.tok.payload) != CondOp::none)) {
        if (!f.in_embed_probe) return condition(f, t);
        report(f, t, lex::LexDiag::embed_cond_operator);
        return t;
      }
      // _Pragma is carried out on rescanning, not while an argument is replaced ([cpp.pragma.op]).
      if (s_->spellings.name(t.tok.payload) == "_Pragma" && f.replacing_argument == 0) {
        if (f.guard_stage != 1) f.guard_stage = -1;
        pragma_operator(f, t);
        continue;
      }
      std::shared_ptr<const MacroDef> def = lookup(t.tok.payload);
      if (!def) return t;
      if (def->dynamic != MacroDef::Dynamic::none) return dynamic(f, t, *def);
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
        replace(f, t, *def, args, written(f, close) + 1);
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
      ++f.replacing_argument;
      a.replaced = expand_line(f, std::move(toks));
      --f.replacing_argument;
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
      if (f.guard_stage != 1) f.guard_stage = -1;
      f.out.tokens.push_back(t);
    }
    // [cpp.cond]/? each #if is matched by an #endif in the same file.
    if (!fatal_)  // after a missing include, as Clang and GCC do, nothing more is reported
      for (const Frame::Cond& c : f.conds) report(f, c.offset, lex::LexDiag::cond_unterminated);
    f.conds.clear();
    if (f.guard_stage == 2) f.out.guard = f.guard;
  }

  // --- directives ([cpp.pre]) ------------------------------------------------------------------

  void directive(Frame& f, const PTok& intro) {
    if (intro.tok.kind != Tk::hash) return module_line(f, intro);
    PTok d = lex_next(f);
    if (!same_line(d)) {  // the null directive
      f.pending.push_front(d);
      return;
    }
    std::string_view word = d.tok.kind == Tk::identifier ? s_->spellings.name(d.tok.payload) : std::string_view();
    // The include guard: only `#ifndef X` first, and its `#endif` last, at the outermost level;
    // any other directive there means the file is more than its guarded group.
    if (f.conds.empty() && !(f.guard_stage == 0 && word == "ifndef") && f.guard_stage != 1) f.guard_stage = -1;
    if (word == "include") return include(f, d, IncludeForm{false, false});
    if (word == "include_next") return include(f, d, IncludeForm{false, true});
    if (word == "define") return define(f, d);
    if (word == "undef") return undef(f, d);
    if (word == "pragma") return pragma(f, intro, d);
    if (word == "if" || word == "ifdef" || word == "ifndef") return cond_if(f, d, word);
    if (word == "elif" || word == "elifdef" || word == "elifndef" || word == "else" || word == "endif")
      return cond_else(f, d, word);
    if (word == "line") return line_directive(f, d);
    if (word == "error" || word == "warning") return diagnostic_directive(f, d, word == "error");
    if (word == "embed") return embed(f, intro, d);
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

  // --- dynamic macros, #line, #error, _Pragma, module lines --------------------------------------

  Fp128 time_answer() const { return Hasher{}.bytes(s_->build_time.first).bytes(s_->build_time.second).finish(); }

  // The physical line of an offset in the frame's file.
  u32 physical_line(Frame& f, u32 offset) {
    if (f.line_starts.empty()) f.line_starts = line_starts_of(f.text);
    return static_cast<u32>(std::ranges::upper_bound(f.line_starts, offset) - f.line_starts.begin());
  }
  static std::vector<u32> line_starts_of(std::string_view text) {
    std::vector<u32> starts{0};
    for (std::size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n' || (text[i] == '\r' && (i + 1 == text.size() || text[i + 1] != '\n')))
        starts.push_back(static_cast<u32>(i + 1));
    return starts;
  }
  static std::string quoted(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
      if (c == '"' || c == '\\') out += '\\';
      out += c;
    }
    return out + '"';
  }

  // [cpp.predefined]: the replacement of a dynamic macro, in an expansion of kind builtin at the
  // name.
  PTok dynamic(Frame& f, const PTok& name, const MacroDef& def) {
    using enum MacroDef::Dynamic;
    u32 e = static_cast<u32>(f.out.expansions.size());
    f.out.expansions.push_back(Expansion{ExpansionKind::builtin, def.name, name.loc.expansion, name.loc.file, name.loc.offset,
                                         name.loc.file, name.loc.offset, 0, 0});
    std::string text;
    switch (def.dynamic) {
    case line: {
      // The presumed line where the name's replacement ends: its own line, or that of the `)` of
      // the outermost invocation that produced it, as Clang has it.
      // Walking out: a token written in an argument is on its own line; one a function-like
      // macro's replacement produced, on the line of that invocation's `)`.
      u32 at = site(f, name);
      for (TokLoc l = name.loc; l.expansion != 0;) {
        const Expansion& x = f.out.expansions[l.expansion];
        if (x.kind == ExpansionKind::argument && l.file == 0) {
          at = l.offset;
          break;
        }
        if (x.kind == ExpansionKind::function && x.operand_a != 0) {
          at = x.operand_a - 1;
          break;
        }
        l = TokLoc{x.site_file, x.parent, x.site_offset};
      }
      text = std::to_string(static_cast<std::int64_t>(physical_line(f, at)) + f.line_delta);
      break;
    }
    case file: text = quoted(f.presumed_name ? *f.presumed_name : f.presumed); break;
    case counter:
      ask(Question{Ask::counter, 0, 0, 0, 0, Fp128{counter_, 0}});
      text = std::to_string(counter_++);
      for (auto& fr : frames_) fr->counter_touched = true;
      break;
    case date:
    case time:
      ask(Question{Ask::time, 0, 0, 0, 0, time_answer()});
      text = def.dynamic == date ? s_->build_time.first : s_->build_time.second;
      break;
    case none: std::unreachable();
    }
    PTok t = lex_one(text).value();
    t.tok.flags = u8(name.tok.flags & tok_flag::whitespace);
    t.loc = TokLoc{name.loc.file, e, name.loc.offset};
    return t;
  }

  // [cpp.line]: `#line digit-sequence "s-char-sequence"(opt)`, after macro replacement.
  void line_directive(Frame& f, const PTok& d) {
    std::vector<PTok> line = expand_line(f, rest_of_line(f));
    auto digits = [&](const PTok& t) {
      if (t.tok.kind != Tk::pp_number) return false;
      const std::string& sp = s_->spellings.literal(t.tok.payload).spelling;
      return !sp.empty() && std::ranges::all_of(sp, [](char c) { return c >= '0' && c <= '9'; });
    };
    if (line.empty() || !digits(line[0])) {
      report(f, line.empty() ? d : line[0], lex::LexDiag::line_bad_number);
      return;
    }
    const std::string& sp = s_->spellings.literal(line[0].tok.payload).spelling;
    // [cpp.line]/3: from 1 to 2147483647.
    unsigned long long v = 0;
    auto [ptr, ec] = std::from_chars(sp.data(), sp.data() + sp.size(), v, 10);
    if (ec != std::errc{} || v == 0 || v > 2147483647ull) {
      report(f, line[0], lex::LexDiag::line_bad_number);
      return;
    }
    std::optional<std::string> name;
    std::size_t used = 1;
    if (line.size() > 1) {
      const PTok& s = line[1];
      const std::string* lit = s.tok.kind == Tk::string_lit ? &s_->spellings.literal(s.tok.payload).spelling : nullptr;
      if (!lit || !lit->starts_with('"') || !lit->ends_with('"')) {
        report(f, s, lex::LexDiag::line_bad_name);
        return;
      }
      std::string n;
      for (std::size_t i = 1; i + 1 < lit->size(); ++i) {
        if ((*lit)[i] == '\\' && i + 2 < lit->size()) ++i;
        n += (*lit)[i];
      }
      name = std::move(n);
      used = 2;
    }
    if (line.size() > used) {
      report(f, line[used], lex::LexDiag::line_extra);
      return;
    }
    // The line after the directive has the presumed number `v`.
    u32 next = f.pending.empty() || f.pending.front().tok.kind == Tk::eof ? static_cast<u32>(f.text.size())
                                                                          : f.pending.front().loc.offset;
    u32 next_line = physical_line(f, site(f, d)) + 1;
    f.line_delta = static_cast<std::int32_t>(v) - static_cast<std::int32_t>(next_line);
    if (name) f.presumed_name = *name;
    f.out.lines.push_back({next, f.line_delta, f.presumed_name ? std::optional(s_->spellings.name(*f.presumed_name)) : std::nullopt});
  }

  // [cpp.error]: `#error` is an error, `#warning` a warning; the message is the line's text.
  void diagnostic_directive(Frame& f, const PTok& d, bool error) {
    std::vector<PTok> line = rest_of_line(f);
    std::string text;
    for (const PTok& t : line) {
      if (!text.empty() && t.tok.has(tok_flag::whitespace)) text += ' ';
      text += spelling(t.tok, s_->spellings);
    }
    report(f, d, error ? lex::LexDiag::error_directive : lex::LexDiag::warning_directive, s_->spellings.name(text));
  }

  // [cpp.pragma.op]: `_Pragma ( string-literal )` is destringized and processed as a #pragma.
  void pragma_operator(Frame& f, const PTok& op) {
    PTok open = read(f);
    PTok str = open.tok.kind == Tk::l_paren ? read(f) : open;
    PTok close = str.tok.kind == Tk::string_lit ? read(f) : str;
    const std::string* lit = str.tok.kind == Tk::string_lit ? &s_->spellings.literal(str.tok.payload).spelling : nullptr;
    if (open.tok.kind != Tk::l_paren || !lit || close.tok.kind != Tk::r_paren || !(lit->starts_with('"') || lit->starts_with("L\""))) {
      report(f, op, lex::LexDiag::pragma_operator_malformed);
      // What was read but is not the operand is read again.
      std::vector<PTok> back;
      if (open.tok.kind != Tk::l_paren) back = {open};
      else if (!lit) back = {str};
      else back = {close};
      f.contexts.push_back(Context{std::move(back), 0, std::nullopt, false});
      return;
    }
    std::string_view body(*lit);
    body.remove_prefix(body.starts_with('L') ? 2 : 1);
    body.remove_suffix(1);
    std::string text;
    for (std::size_t i = 0; i < body.size(); ++i) {
      if (body[i] == '\\' && i + 1 < body.size() && (body[i + 1] == '"' || body[i + 1] == '\\')) ++i;
      text += body[i];
    }
    std::vector<PTok> toks = lex_all(text);
    for (PTok& t : toks) t.loc = op.loc;
    if (!toks.empty() && toks[0].tok.kind == Tk::identifier && s_->spellings.name(toks[0].tok.payload) == "once") {
      if (frames_.size() == 1) report(f, op, lex::LexDiag::pragma_once_in_main);
      else mark_once(f.file);
      return;
    }
    Passthrough p{static_cast<u32>(f.out.tokens.size()), {}};
    p.tokens.push_back(PTok{Tok{Tk::hash, tok_flag::line_start, 0}, op.loc, false});
    p.tokens.push_back(PTok{Tok{Tk::identifier, 0, s_->spellings.name("pragma")}, op.loc, false});
    if (!toks.empty()) toks[0].tok.flags |= tok_flag::space_before;
    p.tokens.insert(p.tokens.end(), toks.begin(), toks.end());
    f.out.passthrough.push_back(std::move(p));
  }

  // The preprocessing tokens of a text, in the session's spellings.
  std::vector<PTok> lex_all(std::string_view text) {
    std::vector<lex::LexDiagnostic> diags;
    lex::Source src(text, diags);
    lex::Tables tables;
    lex::Lexer lx(src, tables, diags);
    std::vector<PTok> out;
    for (lex::PpToken t = lx.next(); t.tok.kind != Tk::eof; t = lx.next()) {
      PTok p{t.tok, {}, false};
      p.tok.flags &= u8(tok_flag::alt | tok_flag::space_before);
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
      out.push_back(p);
    }
    return out;
  }

  // [cpp.module], [cpp.import]: a module or import directive's tokens go on to phase 7. The module
  // name is not replaced, and must be followed by `;` or `[`; what follows it is replaced. A header
  // unit import is not supported yet.
  void module_line(Frame& f, const PTok& intro) {
    f.guard_stage = -1;
    std::vector<PTok> line = rest_of_line(f);
    line.insert(line.begin(), intro);
    std::size_t i = is_name(line[0], "export") ? 1 : 0;
    for (std::size_t k = 0; k <= i; ++k) line[k].tok.flags |= tok_flag::directive;
    std::size_t rest = line.size();
    if (is_name(line[i], "import")) {
      if (i + 1 < line.size() && line[i + 1].tok.kind == Tk::header_name) {
        report(f, line[i + 1], lex::LexDiag::header_unit_unsupported);
        return;
      }
      rest = i + 1;
    } else {
      std::size_t k = i + 1;
      auto name_part = [&] {  // identifier ( . identifier )*
        for (;;) {
          if (k >= line.size() || line[k].tok.kind != Tk::identifier) return false;
          if (auto def = lookup(line[k].tok.payload); def && !def->function_like) report(f, line[k], lex::LexDiag::module_name_macro);
          ++k;
          if (k < line.size() && line[k].tok.kind == Tk::period) ++k;
          else return true;
        }
      };
      if (k < line.size() && line[k].tok.kind == Tk::semi) {
        rest = k;  // `module ;`
      } else if (k + 1 < line.size() && line[k].tok.kind == Tk::colon && is_name(line[k + 1], "private")) {
        rest = k + 2;  // `module : private`
      } else if (name_part() && (k >= line.size() || line[k].tok.kind != Tk::colon || (++k, name_part()))) {
        rest = k;
        // [cpp.module]: the name is followed by `;` or `[`, after replacement, perhaps on a later
        // line.
        std::vector<PTok> tail(line.begin() + std::ptrdiff_t(k), line.end());
        std::vector<PTok> replaced = expand_line(f, tail);
        const PTok& after = !replaced.empty() ? replaced[0] : peek(f, 0);
        if (after.tok.kind != Tk::semi && after.tok.kind != Tk::l_square) report(f, after, lex::LexDiag::module_name_end);
      } else {
        report(f, k < line.size() ? line[k] : line.back(), lex::LexDiag::module_name_end);
        rest = std::min(k, line.size());
      }
    }
    for (std::size_t k = 0; k < rest && k < line.size(); ++k) f.out.tokens.push_back(line[k]);
    std::vector<PTok> tail(line.begin() + std::ptrdiff_t(std::min(rest, line.size())), line.end());
    for (PTok& t : expand_line(f, std::move(tail))) f.out.tokens.push_back(t);
  }

  // --- conditional inclusion ([cpp.cond]) -----------------------------------------------------

  // `#if`, `#ifdef`, `#ifndef`: a group, included if its condition holds, else skipped to the
  // next `#elif`, `#else` or `#endif` of the same level.
  void cond_if(Frame& f, const PTok& d, std::string_view word) {
    std::vector<PTok> line = rest_of_line(f, word == "if");
    bool v = word == "if" ? evaluate(f, d, std::move(line)) : test_defined(f, d, line, word == "ifndef");
    if (word == "ifndef" && f.guard_stage == 0 && f.conds.empty() && line.size() == 1 && line[0].tok.kind == Tk::identifier) {
      f.guard_stage = 1;
      f.guard = line[0].tok.payload;
    } else if (f.conds.empty()) {
      f.guard_stage = -1;
    }
    f.conds.push_back({v, false, site(f, d)});
    if (!v) skip_groups(f);
  }

  // `#ifdef`, `#ifndef`, `#elifdef`, `#elifndef`: whether the name is (`!negate`) a macro.
  bool test_defined(Frame& f, const PTok& d, const std::vector<PTok>& line, bool negate) {
    if (line.empty() || line[0].tok.kind != Tk::identifier) {
      report(f, line.empty() ? d : line[0], lex::LexDiag::macro_name_missing);
      return false;
    }
    extra_tokens(f, line, 1);
    return is_defined(line[0].tok.payload) != negate;
  }

  // `defined X` ([cpp.cond]/? ): the operators `__has_include` and the like count as defined.
  bool is_defined(u32 name) {
    if (condition_operator(name) != CondOp::none) return true;
    bool v = macros_.contains(name);
    ask(Question{Ask::defined, name, 0, 0, 0, Fp128{v ? 1u : 0u, 0}});
    return v;
  }

  // `#elif`, `#elifdef`, `#elifndef`, `#else`, `#endif` reached in an included group: the group
  // ends, and the rest of the #if is skipped.
  void cond_else(Frame& f, const PTok& d, std::string_view word) {
    if (f.conds.empty()) {
      report(f, d, word == "endif" ? lex::LexDiag::cond_endif_without_if : lex::LexDiag::cond_else_without_if);
      rest_of_line(f);
      return;
    }
    Frame::Cond& c = f.conds.back();
    if (word == "endif") {
      extra_tokens(f, rest_of_line(f), 0);
      f.conds.pop_back();
      if (f.conds.empty() && f.guard_stage == 1) f.guard_stage = 2;
      return;
    }
    if (f.conds.size() == 1 && f.guard_stage == 1) f.guard_stage = -1;
    if (c.in_else) report(f, d, word == "else" ? lex::LexDiag::cond_else_after_else : lex::LexDiag::cond_elif_after_else);
    if (word == "else") {
      extra_tokens(f, rest_of_line(f), 0);
      c.in_else = true;
    } else {
      rest_of_line(f, word == "elif");  // not evaluated: a group was taken
    }
    skip_groups(f);
  }

  // Skips the lines of groups not included, up to the group to include or the #if's `#endif`.
  // Only conditional directives are looked at. Skipped lines are still lexed, and what phase 3
  // reports there stands ([lex.pptoken]/2: an unmatched ' is ill-formed, skipped or not).
  void skip_groups(Frame& f) {
    std::size_t depth = 0;
    for (;;) {
      if (fatal_) return;
      PTok t = lex_next(f);
      if (t.tok.kind == Tk::eof) {
        f.pending.push_front(t);
        return;
      }
      if (t.tok.kind != Tk::hash || !t.tok.has(tok_flag::line_start)) {
        rest_of_line(f);
        continue;
      }
      PTok d = lex_next(f);
      if (!same_line(d)) {
        f.pending.push_front(d);
        continue;
      }
      std::string_view word = d.tok.kind == Tk::identifier ? s_->spellings.name(d.tok.payload) : std::string_view();
      auto skip_line = [&] {
        if (word == "embed") {
          // Phase 3 must still see the header name: a slash inside it cannot open a comment.
          PTok header = lex_next(f, true);
          if (!same_line(header)) {
            f.pending.push_front(header);
            return;
          }
        }
        rest_of_line(f, word == "if" || word == "elif" || word == "embed");
      };
      if (word == "if" || word == "ifdef" || word == "ifndef") {
        ++depth;
        skip_line();
        continue;
      }
      if (depth > 0) {
        if (word == "endif") --depth;
        skip_line();
        continue;
      }
      Frame::Cond& c = f.conds.back();
      if (word == "endif") {
        extra_tokens(f, rest_of_line(f), 0);
        f.conds.pop_back();
        if (f.conds.empty() && f.guard_stage == 1) f.guard_stage = 2;
        return;
      }
      if (word != "else" && word != "elif" && word != "elifdef" && word != "elifndef") {
        skip_line();
        continue;
      }
      if (f.conds.size() == 1 && f.guard_stage == 1) f.guard_stage = -1;
      if (c.in_else) report(f, d, word == "else" ? lex::LexDiag::cond_else_after_else : lex::LexDiag::cond_elif_after_else);
      if (word == "else") {
        extra_tokens(f, rest_of_line(f), 0);
        c.in_else = true;
        if (!c.taken) {
          c.taken = true;
          return;
        }
        continue;
      }
      std::vector<PTok> line = rest_of_line(f, word == "elif");
      if (c.taken) continue;  // [cpp.cond]/? not evaluated once a group was included
      bool v = word == "elif" ? evaluate(f, d, std::move(line)) : test_defined(f, d, line, word == "elifndef");
      if (v) {
        c.taken = true;
        return;
      }
    }
  }

  // [cpp.cond]/12–14: the line macro-replaced, with `defined` and the `__has_*` operators
  // applied where they occur, then evaluated.
  bool evaluate(Frame& f, const PTok& d, std::vector<PTok> line) {
    bool saved = std::exchange(f.in_condition, true);
    std::vector<PTok> toks = expand_line(f, std::move(line));
    f.in_condition = saved;
    if (toks.empty()) {
      report(f, d, lex::LexDiag::cond_empty);
      return false;
    }
    Condition c(toks, s_->spellings, s_->config.target->char_signed, [&](const PTok& at, lex::LexDiag k) { report(f, at, k); });
    return c.run().value_or(false);
  }

  // The operators of a #if expression that are not macros ([cpp.cond]/1).
  enum class CondOp { none, defined, has_include, has_include_next, has_embed, has_attribute, has_builtin };
  CondOp condition_operator(u32 name) const {
    std::string_view n = s_->spellings.name(name);
    if (n == "__has_include") return CondOp::has_include;
    if (n == "__has_include_next") return CondOp::has_include_next;
    if (n == "__has_embed") return CondOp::has_embed;
    // DECISIONS §1.5: `__has_builtin` reports the built-ins ycxx supports on this target.
    if (n == "__has_builtin") return CondOp::has_builtin;
    if (n == "__has_cpp_attribute" || n == "__has_attribute" || n == "__has_c_attribute" ||
        n == "__has_feature" || n == "__has_extension")
      return CondOp::has_attribute;
    return CondOp::none;
  }

  // A number for a #if expression.
  PTok number_token(const PTok& at, u64 v) {
    Literal l;
    l.spelling = std::to_string(v);
    return PTok{Tok{Tk::pp_number, u8(at.tok.flags & tok_flag::whitespace), s_->spellings.literal(std::move(l))}, at.loc, false};
  }

  // Applies `defined` or a `__has_*` operator whose name `t` was read in a #if expression; its
  // operand is read unreplaced (or, for a header, replaced if it is not one already).
  PTok condition(Frame& f, const PTok& t) {
    std::string_view n = s_->spellings.name(t.tok.payload);
    if (n == "defined") {
      PTok a = read(f);
      bool paren = a.tok.kind == Tk::l_paren;
      if (paren) a = read(f);
      if (a.tok.kind != Tk::identifier) {
        report(f, a.tok.kind == Tk::eof ? t : a, lex::LexDiag::cond_defined_operand);
        return number_token(t, 0);
      }
      if (paren) {
        PTok close = read(f);
        if (close.tok.kind != Tk::r_paren) report(f, close.tok.kind == Tk::eof ? a : close, lex::LexDiag::cond_expected_rparen);
      }
      return number_token(t, is_defined(a.tok.payload) ? 1 : 0);
    }
    CondOp op = condition_operator(t.tok.payload);
    if (op == CondOp::has_embed) return has_embed(f, t);
    // `__has_x ( balanced tokens )`.
    PTok open = read(f);
    if (open.tok.kind != Tk::l_paren) {
      report(f, open.tok.kind == Tk::eof ? t : open, lex::LexDiag::cond_expected_lparen);
      return number_token(t, 0);
    }
    std::vector<PTok> operand;
    for (std::size_t depth = 0;;) {
      PTok a = read(f);
      if (a.tok.kind == Tk::eof) {
        report(f, t, lex::LexDiag::cond_expected_rparen);
        return number_token(t, 0);
      }
      if (a.tok.kind == Tk::l_paren) ++depth;
      if (a.tok.kind == Tk::r_paren && depth-- == 0) break;
      operand.push_back(a);
    }
    if (op == CondOp::has_attribute) return number_token(t, 0);
    if (op == CondOp::has_builtin) {
      // Its operand is one identifier ([cpp.cond]-style, as GCC and Clang take it).
      if (operand.size() != 1 || operand[0].tok.kind != Tk::identifier) {
        report(f, operand.empty() ? t : operand.size() > 1 ? operand[1] : operand[0], lex::LexDiag::cond_expected_rparen);
        return number_token(t, 0);
      }
      std::string_view name = s_->spellings.name(operand[0].tok.payload);
      const bool variadic = lex::va_builtin(name).has_value();
      const auto* function = lex::builtin_function(name);
      const bool ordinary = function && (function->type != 'l' ||
          ast::computes((*s_->config.target)[ast::Builtin::long_double].format));
      const auto* trait = lex::type_trait(name);
      const bool supported = (trait && trait->advertised) || lex::builtin_template(name) || ordinary || variadic;
      return number_token(t, supported ? 1 : 0);
    }
    // [cpp.cond]/? the header, as for #include: a string literal, `<...>`, or tokens that become
    // one after replacement.
    if (operand.empty() || (operand[0].tok.kind != Tk::string_lit && operand[0].tok.kind != Tk::less))
      operand = expand_line(f, std::move(operand));
    std::size_t used = 0;
    std::string spelled = header_spelling(operand, used);
    if (spelled.empty()) {
      report(f, operand.empty() ? t : operand[0], lex::LexDiag::include_expected);
      return number_token(t, 0);
    }
    if (used != operand.size()) {
      report(f, operand[used], lex::LexDiag::extra_tokens);
      return number_token(t, 0);
    }
    IncludeForm form{spelled.starts_with('<'), op == CondOp::has_include_next};
    std::string_view header = std::string_view(spelled).substr(1, spelled.size() - 2);
    auto r = resolve(s_->files, s_->config, header, form, f.here, f.found_dir);
    ask(Question{Ask::include, s_->spellings.name(spelled), u32(form.angled) | u32(form.next) << 1,
                 std::to_underlying(f.file), f.found_dir, include_answer(r)});
    return number_token(t, r ? 1 : 0);
  }

  enum class EmbedParam { limit, offset, prefix, suffix, if_empty };
  struct EmbedParams {
    u64 limit = std::numeric_limits<u64>::max();
    u64 offset = 0;
    std::span<const PTok> prefix, suffix, if_empty;
    bool supported = true;
  };

  // [cpp.pre]: balanced preprocessing tokens match parentheses, brackets and braces.
  std::span<const PTok> embed_argument(std::span<const PTok> line, std::size_t& i) {
    const std::size_t begin = ++i;
    std::vector<Tk> closing{Tk::r_paren};
    for (; i < line.size(); ++i) {
      switch (Tk kind = line[i].tok.kind) {
      case Tk::l_paren: closing.push_back(Tk::r_paren); break;
      case Tk::l_square: closing.push_back(Tk::r_square); break;
      case Tk::l_brace: closing.push_back(Tk::r_brace); break;
      case Tk::r_paren:
      case Tk::r_square:
      case Tk::r_brace:
        if (closing.back() != kind) return {};
        closing.pop_back();
        if (closing.empty()) return line.subspan(begin, i++ - begin);
        break;
      default: break;
      }
    }
    return {};
  }

  std::optional<EmbedParams> embed_parameters(Frame& f, std::span<const PTok> line, bool probe) {
    EmbedParams result;
    std::set<EmbedParam> seen;
    for (std::size_t i = 0; i < line.size();) {
      const PTok& at = line[i++];
      if (at.tok.kind != Tk::identifier) {
        report(f, at, lex::LexDiag::embed_bad_parameter);
        return std::nullopt;
      }
      std::string_view name = s_->spellings.name(at.tok.payload);
      std::optional<EmbedParam> parameter;
      const bool scoped = i < line.size() && line[i].tok.kind == Tk::coloncolon;
      if (scoped) {
        ++i;
        if (i == line.size() || line[i++].tok.kind != Tk::identifier) {
          report(f, at, lex::LexDiag::embed_bad_parameter);
          return std::nullopt;
        }
      } else if (name == "limit") parameter = EmbedParam::limit;
      else if (name == "offset") parameter = EmbedParam::offset;
      else if (name == "prefix") parameter = EmbedParam::prefix;
      else if (name == "suffix") parameter = EmbedParam::suffix;
      else if (name == "if_empty") parameter = EmbedParam::if_empty;

      std::span<const PTok> argument;
      if (i < line.size() && line[i].tok.kind == Tk::l_paren) {
        argument = embed_argument(line, i);
        if (argument.data() == nullptr) {
          report(f, at, lex::LexDiag::embed_bad_parameter);
          return std::nullopt;
        }
      } else if (parameter || !scoped) {
        report(f, at, lex::LexDiag::embed_bad_parameter);
        return std::nullopt;
      }
      if (!parameter) {
        if (!probe) {
          report(f, at, lex::LexDiag::embed_unsupported_parameter);
          return std::nullopt;
        }
        result.supported = false;
        continue;
      }
      if (*parameter == EmbedParam::prefix || *parameter == EmbedParam::suffix || *parameter == EmbedParam::if_empty) {
        for (const PTok& token : argument) {
          if (token.tok.kind == Tk::header_name || is_name(token, "__has_include") ||
              is_name(token, "__has_embed") || is_name(token, "__has_cpp_attribute")) {
            report(f, token, lex::LexDiag::embed_cond_operator);
            return std::nullopt;
          }
        }
      }
      if (!seen.insert(*parameter).second) {
        report(f, at, lex::LexDiag::embed_duplicate_parameter);
        return std::nullopt;
      }
      switch (*parameter) {
      case EmbedParam::prefix: result.prefix = argument; break;
      case EmbedParam::suffix: result.suffix = argument; break;
      case EmbedParam::if_empty: result.if_empty = argument; break;
      case EmbedParam::limit:
      case EmbedParam::offset: {
        if (argument.empty()) {
          report(f, at, lex::LexDiag::cond_expected_value);
          return std::nullopt;
        }
        for (const PTok& tok : argument) {
          if (*parameter == EmbedParam::limit && is_name(tok, "defined")) {
            report(f, tok, lex::LexDiag::embed_cond_operator);
            return std::nullopt;
          }
        }
        std::vector<PTok> with_operators;
        if (!probe && std::ranges::any_of(argument, [&](const PTok& token) {
              return token.tok.kind == Tk::identifier && condition_operator(token.tok.payload) != CondOp::none;
            })) {
          // Normal macro replacement already ran. Only apply conditional operators now.
          with_operators.assign(argument.begin(), argument.end());
          for (PTok& token : with_operators)
            token.painted = token.tok.kind == Tk::identifier && condition_operator(token.tok.payload) == CondOp::none;
          const bool saved = std::exchange(f.in_condition, true);
          with_operators = expand_line(f, std::move(with_operators));
          f.in_condition = saved;
          argument = with_operators;
        }
        Condition expression(argument, s_->spellings, s_->config.target->char_signed,
                             [&](const PTok& token, lex::LexDiag k) { report(f, token, k); });
        auto value = expression.value();
        if (!value) return std::nullopt;
        if (!value->uns && static_cast<std::int64_t>(value->bits) < 0) {
          report(f, at, lex::LexDiag::embed_negative);
          return std::nullopt;
        }
        (*parameter == EmbedParam::limit ? result.limit : result.offset) = value->bits;
        break;
      }
      }
    }
    return result;
  }

  struct EmbedResource {
    FileId file;
    std::string_view bytes;
  };

  std::optional<EmbedResource> embed_bytes(Frame& f, std::string_view spelled, const EmbedParams& params) {
    // A resource uses the same local and configured search paths as a header. Both the
    // resolution (including a miss) and the bytes are dependencies of the region.
    IncludeForm form{spelled.starts_with('<'), false};
    auto resolved = resolve(s_->files, s_->config, spelled.substr(1, spelled.size() - 2), form, f.here, f.found_dir);
    ask(Question{Ask::include, s_->spellings.name(spelled), u32(form.angled),
                 std::to_underlying(f.file), f.found_dir, include_answer(resolved)});
    if (!resolved) return std::nullopt;
    const Files::Content* c = s_->files.content(resolved->file);
    ask(Question{Ask::content, std::to_underlying(resolved->file), 0, 0, 0, c ? c->fp : Fp128{}});
    if (!c) return std::nullopt;
    std::string_view bytes(c->text);
    bytes.remove_prefix(std::min<u64>(params.offset, bytes.size()));
    return EmbedResource{resolved->file, bytes.substr(0, params.limit)};
  }

  static bool valid_embed_name(std::string_view spelled) {
    return spelled.size() >= 3 && spelled.find(spelled.front() == '<' ? '>' : '"', 1) == spelled.size() - 1;
  }

  // [cpp.cond]: replace the operand once. Directly written resource names are already
  // header-name tokens; the interiors of macro-produced <...> token sequences are replaced.
  PTok has_embed(Frame& f, const PTok& t) {
    const bool saved = std::exchange(f.in_condition, false);
    PTok open = next(f);
    f.in_condition = saved;
    if (open.tok.kind != Tk::l_paren) {
      report(f, open.tok.kind == Tk::eof ? t : open, lex::LexDiag::cond_expected_lparen);
      return number_token(t, 0);
    }
    std::vector<PTok> line;
    f.in_embed_probe = true;
    bool angle = false;
    std::size_t depth = 0;
    for (;;) {
      PTok at = next(f);
      if (at.tok.kind == Tk::eof) {
        f.in_embed_probe = false;
        report(f, t, lex::LexDiag::cond_expected_rparen);
        return number_token(t, 0);
      }
      if (line.empty() && at.tok.kind == Tk::less) angle = true;
      if (angle) {
        if (at.tok.kind == Tk::greater) angle = false;
      } else {
        if (at.tok.kind == Tk::r_paren) {
          if (depth == 0) break;
          --depth;
        }
        if (at.tok.kind == Tk::l_paren) ++depth;
      }
      line.push_back(at);
    }
    f.in_embed_probe = false;
    std::size_t used = 0;
    std::string spelled = header_spelling(line, used);
    if (!valid_embed_name(spelled)) {
      report(f, line.empty() ? t : line[0], lex::LexDiag::include_expected);
      return number_token(t, 0);
    }
    auto params = embed_parameters(f, std::span(line).subspan(used), true);
    if (!params || !params->supported) return number_token(t, 0);
    auto resource = embed_bytes(f, spelled, *params);
    return number_token(t, resource ? resource->bytes.empty() ? 2 : 1 : 0);
  }

  // [cpp.embed]: a direct resource name is lexed as such; otherwise replace the entire line.
  void embed(Frame& f, const PTok& intro, const PTok& d) {
    PTok header = lex_next(f, true);
    const bool direct = header.tok.kind == Tk::header_name && same_line(header);
    if (!direct) f.pending.push_front(header);
    std::vector<PTok> line = rest_of_line(f, true);
    if (f.collecting > 0) {
      report(f, d, lex::LexDiag::embed_in_arguments);
      return;
    }
    for (const PTok& token : line) {
      if (token.tok.kind == Tk::identifier && embed_macro_name(s_->spellings.name(token.tok.payload)) &&
          is_defined(token.tok.payload)) {
        report(f, token, lex::LexDiag::embed_parameter_macro);
        return;
      }
    }
    line = expand_line(f, std::move(line), direct ? LineMode::embed_params : LineMode::embed_name);
    std::size_t used = 0;
    std::string spelled = direct ? std::string(s_->spellings.name(header.tok.payload)) : header_spelling(line, used);
    if (!direct && !line.empty()) header = line[0];
    if (!valid_embed_name(spelled)) {
      report(f, same_line(header) ? header : d, lex::LexDiag::include_expected);
      return;
    }
    auto params = embed_parameters(f, std::span(line).subspan(used), false);
    if (!params) return;
    auto resource = embed_bytes(f, spelled, *params);
    if (!resource) {
      report(f, header, lex::LexDiag::embed_not_found);
      return;
    }
    if (resource->bytes.empty() && params->if_empty.empty()) return;
    u16 file = file_index(f, resource->file, s_->spellings.name(s_->files.path(resource->file)));
    u32 expansion = static_cast<u32>(f.out.expansions.size());
    f.out.expansions.push_back(Expansion{ExpansionKind::embed, d.tok.payload, 0, intro.loc.file,
                                        intro.loc.offset, file});
    // The operand was already replaced as normal text. Keep the replacement together at
    // the directive's site and the spelling locations of the parameter tokens.
    auto append = [&](std::span<const PTok> tokens) {
      for (PTok token : tokens) {
        TokLoc loc = token.loc;
        u32 parameter_expansion = static_cast<u32>(f.out.expansions.size());
        f.out.expansions.push_back(Expansion{ExpansionKind::argument, d.tok.payload, expansion,
                                            intro.loc.file, intro.loc.offset, loc.file, loc.offset});
        token.loc.expansion = parameter_expansion;
        f.out.tokens.push_back(token);
      }
    };
    if (resource->bytes.empty()) {
      append(params->if_empty);
    } else {
      append(params->prefix);
      f.out.tokens.push_back(PTok{Tok{Tk::embed, intro.tok.flags, s_->spellings.resource(resource->bytes)},
                                  TokLoc{intro.loc.file, expansion, intro.loc.offset}, false});
      append(params->suffix);
    }
  }

  // A header name from tokens: a string literal, or the spellings between `<` and `>` joined
  // where they were apart ([cpp.include]/4); empty if they form none. `used` counts its tokens.
  std::string header_spelling(const std::vector<PTok>& line, std::size_t& used) {
    used = 0;
    if (line.empty()) return {};
    if (line[0].tok.kind == Tk::header_name) {
      used = 1;
      return std::string(s_->spellings.name(line[0].tok.payload));
    }
    if (line[0].tok.kind == Tk::string_lit) {
      const std::string& sp = s_->spellings.literal(line[0].tok.payload).spelling;
      if (!sp.starts_with('"') || !sp.ends_with('"')) return {};
      used = 1;
      return sp;
    }
    if (line[0].tok.kind != Tk::less) return {};
    std::string spelled = "<";
    std::size_t i = 1;
    for (; i < line.size() && line[i].tok.kind != Tk::greater; ++i) {
      if (i > 1 && line[i].tok.has(tok_flag::space_before)) spelled += ' ';
      spelled += spelling(line[i].tok, s_->spellings);
    }
    if (i == line.size() || i == 1) return {};
    used = i + 1;
    return spelled + '>';
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
      spelled = header_spelling(line, used);
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
    std::shared_ptr<const FileRegion> child =
        enter(r->file, presumed, r->dir, s_->config.system_header(r->dir, f.system), *c, bounds);
    // Placed after the directive's own diagnostics, at its last token.
    u32 end = line.empty() ? site(f, h) : site(f, line.back());
    f.out.includes.push_back(Include{at, end, std::move(child), std::move(bounds)});
  }

  enum class LineMode { normal, embed_name, embed_params };

  // A directive's tokens, fully macro-replaced; replacement does not read past them.
  std::vector<PTok> expand_line(Frame& f, std::vector<PTok> toks, LineMode mode = LineMode::normal) {
    auto saved = std::exchange(f.contexts, {});
    bool space = std::exchange(f.pending_space, false);
    f.contexts.push_back(Context{std::move(toks), 0, std::nullopt, true});
    std::vector<PTok> out;
    const bool condition = f.in_condition;
    bool angle = false, offset = false;
    std::size_t depth = 0;
    for (;;) {
      PTok t = next(f);
      Tk kind = t.tok.kind;
      if (kind == Tk::eof) break;
      if (mode == LineMode::embed_name) {
        if (out.empty()) angle = kind == Tk::less;
        if (!angle || kind == Tk::greater) mode = LineMode::embed_params;
      } else if (mode == LineMode::embed_params) {
        if (depth == 0) {
          // Offset uses [cpp.cond], including a defined operand protected from macro replacement.
          // Limit's explicit ban on defined is checked when its expression is parsed.
          if (kind == Tk::l_paren) f.in_condition = offset;
          offset = is_name(t, "offset") && (out.empty() || out.back().tok.kind != Tk::coloncolon);
        }
        if (kind == Tk::l_paren) ++depth;
        if (kind == Tk::r_paren && depth != 0 && --depth == 0) f.in_condition = condition;
      }
      out.push_back(t);
    }
    f.in_condition = condition;
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
  u32 counter_ = 0;  // `__COUNTER__`
  u32 va_args_ = s_->spellings.name("__VA_ARGS__");
  u32 va_opt_ = s_->spellings.name("__VA_OPT__");
  std::vector<std::unique_ptr<Frame>> frames_;
  std::unordered_map<u32, std::shared_ptr<const MacroDef>> macros_;
  std::set<FileId> once_;
  bool fatal_ = false;
};

}  // namespace ycxx::pp
