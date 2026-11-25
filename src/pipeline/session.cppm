// A compilation session (#63): the engine and everything its cells' values point into (the
// interners, sema's world, the optimization units), alive together. `ycxx cxx` builds through
// one in-process; the daemon (#96) keeps one alive between requests, so a second build of an
// unchanged unit executes nothing past its inputs.
//
//   Session s;
//   s.set_source(TuId{0}, "a.cpp", text, "a.o");
//   s.set_program(ProgramId{0}, {{TuId{0}}, {}, "a.out"});
//   const Linked& l = s.get<Link>(ProgramId{0});
export module ycxx.pipeline:session;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.items;
import ycxx.pp;
import ycxx.sema;
import :back;
import :front;
import :policies;
import :policy_objects;
import :executable;
import :lines;

namespace ycxx::pipeline {

// The names of registered programs and requests, for traces (#245): process-wide, as the
// numbers they name are a session's and the daemon has one.
struct TraceNames {
  using Table = std::map<std::uint32_t, std::string>;
  std::mutex m;
  Table programs, requests;
};
TraceNames& trace_names() {
  static TraceNames n;
  return n;
}
void name_for_trace(TraceNames::Table& t, std::uint32_t n, std::string name) {
  std::lock_guard lock(trace_names().m);
  t.insert_or_assign(n, std::move(name));
}

}  // namespace ycxx::pipeline

export namespace ycxx::pipeline {

// One request's work as one root (#139): its units' front ends, then their objects (or none, with
// `-fsyntax-only`), then the program's link. A batch that stops it restarts the whole root, whose
// finished cells survive, so the answer is of one revision (daemon design §4.3).
enum class Until : std::uint8_t { syntax, object, link };
struct BuildKey {
  std::vector<TuId> units;
  Until until = Until::link;
  ProgramId program{};  // with `link`
  bool want_dependencies = false;
  bool check_policies = false;
  bool operator==(const BuildKey&) const = default;
};
// A request, numbered by the registry (`Session::request`): a key holds no vector.
struct RequestId {
  std::uint32_t n = 0;
  bool operator==(const RequestId&) const = default;
};
// A request in engine traces (#245): its units and what it makes, `a.cpp b.cpp → a`.
void trace_describe(std::string& out, RequestId r);
struct [[=engine::input{engine::Durability::low}]] BuildIn {
  using Key = RequestId;
  using Value = BuildKey;
};
struct UnitBuilt {
  bool readable = false;
  std::vector<Message> messages;  // the front end's
  bool front_failed = false;
  std::optional<obj::object> object;  // with `object` or `link`, when the back end succeeded
  std::vector<Message> errors;  // the back end's
  std::vector<std::string> dependencies;  // with `want_dependencies`, when the back end succeeded
};
struct Built {
  std::vector<UnitBuilt> units;
  std::optional<Linked> linked;  // with `link`, when every unit compiled
};
struct Build {
  using Key = RequestId;
  using Value = Built;
  static engine::query_task<Value> run(engine::Ctx& cx, RequestId r);
  static std::string errors(const Value& v) {  // for traces (#245)
    std::string out;
    for (const UnitBuilt& u : v.units) {
      if (!u.readable) out += "cannot read a source file\n";
      for (const Message& m : u.messages) m.append_trace_error(out);
      for (const Message& m : u.errors) m.append_trace_error(out);
    }
    if (v.linked) {
      for (const std::string& e : v.linked->errors) out += e + "\n";
    }
    return out;
  }
};

// A unit named by what it is (#139, daemon design §4.4): its spelled path, the working directory
// it is relative to, its preprocessing configuration and its object's name.
struct UnitSpec {
  std::string path;
  std::string cwd;
  pp::Config config;
  std::string object;
};

// What a request's root did, restarts included (daemon design §8.1).
struct RunStats {
  std::size_t restarts = 0;
  engine::Stats engine;
};

// Optional process-test checkpoint, after the pooled Build has read and analysed its sources.
using BuildCheckpoint = std::function<void(std::string_view, std::inplace_stop_token)>;

class Session {
 public:
  // A session whose queries run on `pool`'s workers, each bound to this session's universe (#137).
  // The pool serves this session only, for as long as it lives.
  explicit Session(engine::Pool& pool, BuildCheckpoint checkpoint = {})
      : db_(pool), pool_(&pool), checkpoint_(std::move(checkpoint)) {
    scope_ = {
        this,
        [](void* self) noexcept {
          auto& s = *static_cast<Session*>(self);
          bind_thread(&s.universe_);
          worker_checkpoint_ = s.checkpoint_ ? &s.checkpoint_ : nullptr;
        },
        [] noexcept {
          bind_thread(nullptr);
          worker_checkpoint_ = nullptr;
        }};
    pool.claim_scope(&scope_);
    epochs_ = std::make_unique<engine::Epochs>(db_);
    init();
  }
  Session() { init(); }
  ~Session() {
    epochs_.reset();  // stops and joins every request before the scope goes
    if (pool_) pool_->release_scope(&scope_);
    forget_group_cache(&universe_);  // its analyses hold this session's handles (#947)
  }
  // How many groups this session's units analysed, and how many they reused (#947).
  std::pair<std::uint64_t, std::uint64_t> group_counts() const { return group_cache_counts(&universe_); }

 private:
  void init() {
    for (obj::target t : {obj::target::aarch64_macos, obj::target::x86_64_linux}) {
      db_.set<CostModelIn>(t, {});
      db_.set<OptCacheSnapshot>(t, {});
      db_.set<OptLevel>(t, 0);
    }
    // Headers are read from the disk unless the owner provides them otherwise (the daemon's file
    // table, daemon design §5.1).
    db_.set_provider<pp::FileContent>(pp::read_disk);
    // No editor buffer is open until the owner sets one (daemon design §6.1).
    db_.set_provider<pp::Buffer>(pp::no_buffer);
    db_.set_provider<pp::GeneratedFile>(pp::no_generated_file);
    // Named units and programs (`unit`, `program`) are provided from the registry.
    db_.set_provider<pp::SourceOf>([this](pp::FileId f) {
      std::lock_guard lock(registry_m_);
      const UnitSpec& u = unit_at(std::to_underlying(f));
      return engine::provided<pp::Source>{{u.path, u.cwd, u.config}, engine::Durability::low};
    });
    db_.set_provider<pp::FileText>([this](pp::FileId f) {
      std::string path;
      {
        std::lock_guard lock(registry_m_);
        path = absolute(unit_at(std::to_underlying(f)));
      }
      return engine::provided<std::string>{read_(path).value_or(std::string{}), engine::Durability::low};
    });
    db_.set_provider<TuSourceIn>([this](TuId tu) {
      std::lock_guard lock(registry_m_);
      return engine::provided<TuSource>{{unit_at(tu.n).object, tu.n, {}}, engine::Durability::low};
    });
    // BOOTSTRAP: contracts: a `contract_assert` in these lambdas reports a violation whose predicate
    // holds (GCC 16.2), so the checks are the members' preconditions.
    db_.set_provider<BuildIn>([this](RequestId r) {
      std::lock_guard lock(registry_m_);
      return engine::provided<BuildKey>{request_at(r.n), engine::Durability::low};
    });
    db_.set_provider<ProgramIn>([this](ProgramId p) {
      std::lock_guard lock(registry_m_);
      return engine::provided<ProgramInputs>{program_at(p.n), engine::Durability::low};
    });
  }

  // Registered units and programs are numbered from here, apart from the numbers tests set.
  static constexpr std::uint32_t registry_base = 1u << 30;
  const UnitSpec& unit_at(std::uint32_t n) const
    pre(n >= registry_base && n - registry_base < named_.size())
  {
    return named_[n - registry_base];
  }
  const BuildKey& request_at(std::uint32_t n) const
    pre(n < requests_.size())
  {
    return requests_[n];
  }
  const ProgramInputs& program_at(std::uint32_t n) const
    pre(n >= registry_base && n - registry_base < programs_.size())
  {
    return programs_[n - registry_base];
  }
  static std::string absolute(const UnitSpec& u) {
    std::filesystem::path p(u.path);
    if (p.is_relative() && !u.cwd.empty()) p = std::filesystem::path(u.cwd) / p;
    return p.lexically_normal().generic_native_encoded_string();
  }

 public:
  Session(const Session&) = delete;

  // A translation unit from a source file: `path` names it (and locates its `#include "..."`),
  // `object` is the object's name; relative paths are relative to `cwd`, and `config` is its
  // preprocessing configuration. The driver's file number is the unit's. Only a disk-view
  // unit uses `text`. The editor and build views read the main file through their path inputs.
  void set_source(TuId tu, std::string path, std::string text, std::string object, std::string cwd = {},
                  pp::Config config = {}) {
    Bind bind(universe_);  // input values may hold handles, fingerprinted by their interners
    const pp::FileId file{tu.n};
    const bool disk = config.view == pp::View::disk;
    pp::name_file(file, std::filesystem::path(path).filename().native_encoded_string());
    db_.set<pp::SourceOf>(file, {std::move(path), std::move(cwd), std::move(config)});
    if (disk) db_.set<pp::FileText>(file, std::move(text));
    db_.set<TuSourceIn>(tu, {std::move(object), tu.n, {}});
  }
  void set_program(ProgramId p, ProgramInputs in) {
    Bind bind(universe_);
    db_.set<ProgramIn>(p, std::move(in));
  }

  // The unit `u` names (#139): the same number for the same unit, in every request, so concurrent
  // requests never set each other's inputs and naming one needs no quiescence. Its inputs are
  // provided from this registry, its main file's text by the session's reader.
  TuId unit(UnitSpec u) {
    Bind bind(universe_);  // a configuration may hold handles, fingerprinted by their interners
    const Fp128 key = fingerprint(std::tuple{pp::Source{u.path, u.cwd, u.config}, u.object});
    std::lock_guard lock(registry_m_);
    auto [it, fresh] = unit_numbers_.try_emplace(key, registry_base + static_cast<std::uint32_t>(named_.size()));
    if (fresh) {
      if (u.config.view == pp::View::disk) by_path_[absolute(u)].push_back(it->second);
      pp::name_file(pp::FileId{it->second}, std::filesystem::path(u.path).filename().native_encoded_string());
      named_.push_back(std::move(u));
    }
    return TuId{it->second};
  }
  // The program `in` names, likewise.
  ProgramId program(ProgramInputs in) {
    Bind bind(universe_);
    const Fp128 key = fingerprint(in);
    std::lock_guard lock(registry_m_);
    auto [it, fresh] = program_numbers_.try_emplace(key, registry_base + static_cast<std::uint32_t>(programs_.size()));
    if (fresh) {
      name_for_trace(trace_names().programs, it->second, in.identifier);
      programs_.push_back(std::move(in));
    }
    return ProgramId{it->second};
  }
  // The request `k` names, likewise.
  RequestId request(BuildKey k) {
    const Fp128 key = fingerprint(k);
    std::lock_guard lock(registry_m_);
    auto [it, fresh] = request_numbers_.try_emplace(key, static_cast<std::uint32_t>(requests_.size()));
    if (fresh) {
      std::string name;
      for (TuId u : k.units) {
        if (!name.empty()) name += ' ';
        if (u.n >= registry_base && u.n - registry_base < named_.size())
          name += std::filesystem::path(named_[u.n - registry_base].path).filename().native_encoded_string();
      }
      if (k.until == Until::syntax) name += " -fsyntax-only";
      else if (k.until == Until::object) name += " -c";
      else if (k.program.n >= registry_base && k.program.n - registry_base < programs_.size())
        name += " → " + programs_[k.program.n - registry_base].identifier;
      if (k.check_policies) name += " -fcheck-policies";
      name_for_trace(trace_names().requests, it->second, std::move(name));
      requests_.push_back(std::move(k));
    }
    return RequestId{it->second};
  }
  // How registered units' main files are read: from the disk unless the owner says otherwise
  // (the daemon's file table). Set before any unit is named.
  void set_reader(std::function<std::optional<std::string>(const std::string&)> read) { read_ = std::move(read); }
  // The file at `absolute` changed: the main-file text of every registered unit of it. At
  // quiescence (inside `apply`); whether any input changed.
  bool set_main_file(const std::string& absolute, const std::optional<std::string>& text) {
    std::vector<std::uint32_t> ns;
    {
      std::lock_guard lock(registry_m_);
      if (auto it = by_path_.find(absolute); it != by_path_.end()) ns = it->second;
    }
    bool changed = false;
    for (std::uint32_t n : ns) changed = db_.set<pp::FileText>(pp::FileId{n}, text.value_or(std::string{})) || changed;
    return changed;
  }

  // Updates a virtual file in the build view. The caller writes a batch under `apply`.
  bool set_generated(std::string_view absolute_path, std::optional<std::string> contents)
    pre(absolute_path.starts_with('/'))
  {
    return db_.set<pp::GeneratedFile>(pp::path_id(pp::Files::normalize(absolute_path)), std::move(contents));
  }

  // A request's root (#139). On a pooled session it is spawned into the current epoch: a batch
  // (`apply`) restarts it where it stopped, and once `cancelled()` (polled) it stops for good and
  // this returns nothing. Otherwise it runs here. The value is copied: a later batch may replace
  // the cell's.
  template <engine::MemoQuery Q>
  std::optional<typename Q::Value> run(const typename Q::Key& k, const std::function<bool()>& cancelled = {},
                                       RunStats* stats = nullptr, engine::Tracer* tracer = nullptr) {
    using V = typename Q::Value;
    if (!epochs_) {
      const engine::Stats before = db_.stats();
      std::optional<V> v{get<Q>(k, tracer)};
      if (stats) {
        const engine::Stats now = db_.stats();
        stats->engine.executed = now.executed - before.executed;
        stats->engine.validated = now.validated - before.validated;
      }
      return v;
    }
    struct Wait {
      std::mutex m;
      std::condition_variable cv;
      std::optional<V> value;
      std::exception_ptr error;
      bool done = false;
    };
    Bind bind(universe_);  // a spawn may start the root on this thread
    auto w = std::make_shared<Wait>();
    auto finish = [w](auto&& set) {
      std::lock_guard lock(w->m);
      set(*w);
      w->done = true;
      w->cv.notify_all();
    };
    struct InFlight {
      Session& s;
      explicit InFlight(Session& s) : s(s) {
        const std::size_t now = s.in_flight_.fetch_add(1) + 1;
        std::size_t peak = s.peak_in_flight_.load();
        while (now > peak && !s.peak_in_flight_.compare_exchange_weak(peak, now)) {}
      }
      ~InFlight() { s.in_flight_.fetch_sub(1); }
    } in_flight{*this};
    auto ticket = epochs_->spawn<Q>(
        k, engine::Priority::interactive, [finish](const V& v) { finish([&](Wait& x) { x.value.emplace(v); }); },
        [finish](std::exception_ptr e) { finish([&](Wait& x) { x.error = std::move(e); }); }, tracer);
    std::unique_lock lock(w->m);
    while (!w->cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return w->done; })) {
      if (!cancelled) continue;
      lock.unlock();
      const bool stop = cancelled();
      lock.lock();
      if (stop && !w->done) {
        // Its frames stop at their next fetch; wait for them, so nothing they use (`tracer`) is
        // gone while they run (#245). Unlocked: a completion takes `w->m`.
        lock.unlock();
        ticket->cancel();
        ticket->wait_idle();
        return std::nullopt;
      }
    }
    if (stats) *stats = {ticket->restarts(), ticket->stats()};
    if (w->error) std::rethrow_exception(w->error);
    return std::move(w->value);
  }

  // The most requests (`run`) that were in the engine at once, and how many are now.
  std::size_t peak_requests() const noexcept { return peak_in_flight_.load(); }
  std::size_t requests_in_flight() const noexcept { return in_flight_.load(); }

  // Changes inputs: `write(db)` runs at quiescence. On a pooled session every running request is
  // stopped first and restarted after, keeping its finished cells (daemon design §4.1).
  template <class F>
  engine::Epochs::Applied apply(F&& write) {
    Bind bind(universe_);
    if (epochs_) return epochs_->apply(std::forward<F>(write));
    std::forward<F>(write)(db_);
    return {};
  }

  // A root request, with the session's universe bound; `tracer` gets its events (#245).
  template <engine::MemoQuery Q>
  decltype(auto) get(const typename Q::Key& k, engine::Tracer* tracer = nullptr) {
    Bind bind(universe_);
    return db_.get<Q>(k, tracer);
  }

  // The byte offset of a location in a source unit's main file.
  std::uint32_t byte_offset(TuId tu, ast::Loc at) {
    const pp::FileId file{tu.n};
    if (const auto loc = source_site(ins_, get<pp::Pp>(file), get<items::RegionItems>(file), at)) return loc->offset;
    return 0;
  }

  engine::Db& db() { return db_; }
  ast::Interners& interners() { return ins_; }

 private:
  friend struct Build;
  inline static thread_local const BuildCheckpoint* worker_checkpoint_ = nullptr;
  ast::Interners ins_;
  sema::World world_{ins_};
  sema::World linux_world_{ins_, ast::x86_64_linux};
  ValueConstants consts_{world_};
  UnitStore units_;
  Universe universe_{&ins_, &consts_, &ast::aarch64_darwin, &world_, &units_, &linux_world_};
  engine::Db db_;
  engine::Pool* pool_ = nullptr;
  BuildCheckpoint checkpoint_;
  engine::Pool::Scope scope_;
  std::unique_ptr<engine::Epochs> epochs_;  // with a pool
  std::atomic<std::size_t> in_flight_{0}, peak_in_flight_{0};

  std::function<std::optional<std::string>(const std::string&)> read_ = [](const std::string& path) {
    return pp::read_disk(pp::path_id(path)).value;
  };
  std::mutex registry_m_;
  std::deque<UnitSpec> named_;  // by number - registry_base
  std::map<Fp128, std::uint32_t> unit_numbers_;
  std::map<std::string, std::vector<std::uint32_t>> by_path_;  // disk-view units by main file
  std::deque<ProgramInputs> programs_;
  std::map<Fp128, std::uint32_t> program_numbers_;
  std::deque<BuildKey> requests_;
  std::map<Fp128, std::uint32_t> request_numbers_;
};

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

engine::query_task<Built> Build::run(engine::Ctx& cx, RequestId r) {
  const BuildKey& k = cx.fetch<BuildIn>(r);
  Built out;
  bool all = true;
  std::vector<std::reference_wrapper<const CheckedPolicies>> policies;
  // Check each main file in this revision (the file table's, in the daemon).
  const pp::Source* first = nullptr;
  std::vector<TuId> readable;
  for (TuId tu : k.units) {
    const pp::Source& src = cx.fetch<pp::SourceOf>(pp::FileId{tu.n});
    if (!first) first = &src;
    UnitBuilt& u = out.units.emplace_back();
    u.readable = pp::source_content(cx, src, src.path).has_value();
    if (u.readable) readable.push_back(tu);
    else all = false;
  }
  {
    const auto fronts = co_await cx.par_map<TuFront>(readable);
    std::size_t j = 0;
    for (UnitBuilt& u : out.units) {
      if (!u.readable) continue;
      const FrontResult& f = fronts[j++];
      u.messages = f.messages;
      u.front_failed = f.has_errors();
      all = all && !f.has_errors();
    }
  }
  if (const auto* checkpoint = Session::worker_checkpoint_; checkpoint && first) {
    (*checkpoint)(first->path, cx.stop_token());
    co_await cx.stop_point();
  }
  if (k.check_policies) {
    std::vector<ast::u32> checked;
    for (std::size_t i = 0; i < k.units.size(); ++i)
      if (out.units[i].readable && !out.units[i].front_failed) checked.push_back(k.units[i].n);
    const auto sources = universe().in->intern_seq<ast::u32>(checked);
    auto effects = EffectVerification::complete_sources;
    if (k.until == Until::object) effects = EffectVerification::deferred_objects;
    if (k.until == Until::link) effects = effect_link_verification(cx.fetch<ProgramIn>(k.program));
    const auto units = checked | std::views::transform([sources, defer = k.until != Until::syntax, effects](ast::u32 unit) {
      return PolicyCheckUnit{sources, TuId{unit}, defer, effects};
    });
    policies = co_await cx.par_map<CheckPolicies>(units);
    std::size_t j = 0;
    for (UnitBuilt& u : out.units) {
      if (!u.readable || u.front_failed) continue;
      const auto& messages = policies[j++].get().messages;
      u.messages.append_range(messages);
      u.front_failed = !messages.empty();
      all = all && !u.front_failed;
    }
    // A failed input can withhold part of the policy used to check every other input.
    if (!all) co_return out;
  }
  if (k.until == Until::syntax) co_return out;
  std::vector<Tu::Key> compiled;
  for (std::size_t i = 0; i < k.units.size(); ++i)
    if (out.units[i].readable && !out.units[i].front_failed) compiled.push_back({k.units[i], k.check_policies});
  {
    const auto tus = co_await cx.par_map<Tu>(compiled);
    std::size_t j = 0;
    for (std::size_t i = 0; i < k.units.size(); ++i) {
      if (!out.units[i].readable || out.units[i].front_failed) continue;
      const TuValue& v = tus[j++];
      UnitBuilt& u = out.units[i];
      u.object = v.object;
      if (k.want_dependencies && u.object) {
        const pp::Region& region = co_await cx.fetch<pp::Pp>(pp::FileId{k.units[i].n});
        u.dependencies = region.dependencies;
      }
      all = all && v.object.has_value();
      if (u.object && k.check_policies && k.until == Until::object) {
        if (auto error = embed_policy_contracts(*u.object, v, policies[i].get().contracts)) {
          u.errors.push_back({std::nullopt, true, std::move(*error), {}});
          u.object.reset();
          all = false;
        }
      }
      for (const BackError& e : v.errors) {
        if (e.source) u.errors.push_back({e.source->offset, true, e.message, e.source->where});
        else u.errors.push_back({std::nullopt, true, e.message, {}});
      }
    }
  }
  if (k.until == Until::link && all) out.linked = co_await cx.fetch<Link>(k.program);
  co_return out;
}

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

void trace_describe(std::string& out, TuId tu) { trace_describe(out, pp::FileId{tu.n}); }

namespace {
void describe_named(std::string& out, TraceNames::Table& t, std::uint32_t n) {
  std::lock_guard lock(trace_names().m);
  if (const auto it = t.find(n); it != t.end()) out += it->second;
  else out += std::to_string(n);
}
}  // namespace

void trace_describe(std::string& out, ProgramId p) { describe_named(out, trace_names().programs, p.n); }
void trace_describe(std::string& out, RequestId r) { describe_named(out, trace_names().requests, r.n); }

}  // namespace ycxx::pipeline
