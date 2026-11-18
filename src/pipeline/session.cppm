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
import :executable;

export namespace ycxx::pipeline {

// One request's work as one root (#139): its units' front ends, then their objects (or none, with
// `-fsyntax-only`), then the program's link. A batch that stops it restarts the whole root, whose
// finished cells survive, so the answer is of one revision (daemon design §4.3).
enum class Until : std::uint8_t { syntax, object, link };
struct BuildKey {
  std::vector<TuId> units;
  Until until = Until::link;
  ProgramId program{};  // with `link`
  bool operator==(const BuildKey&) const = default;
};
// A request, numbered by the registry (`Session::request`): a key holds no vector.
struct RequestId {
  std::uint32_t n = 0;
  bool operator==(const RequestId&) const = default;
};
struct [[=engine::input{engine::Durability::low}]] BuildIn {
  using Key = RequestId;
  using Value = BuildKey;
};
struct UnitBuilt {
  std::vector<Message> messages;  // the front end's
  bool front_failed = false;
  std::optional<obj::object> object;  // with `object` or `link`, when the back end succeeded
  // The back end's errors: a byte offset in the unit's main file, when there is a location.
  std::vector<std::pair<std::optional<std::uint32_t>, std::string>> errors;
};
struct Built {
  std::vector<UnitBuilt> units;
  std::optional<Linked> linked;  // with `link`, when every unit compiled
};
struct Build {
  using Key = RequestId;
  using Value = Built;
  static engine::query_task<Value> run(engine::Ctx& cx, RequestId r);
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

class Session {
 public:
  // A session whose queries run on `pool`'s workers, each bound to this session's universe (#137).
  // The pool serves this session only, for as long as it lives.
  explicit Session(engine::Pool& pool) : db_(pool), pool_(&pool) {
    scope_ = {this, [](void* self) noexcept { bind_thread(&static_cast<Session*>(self)->universe_); },
              [] noexcept { bind_thread(nullptr); }};
    pool.claim_scope(&scope_);
    epochs_ = std::make_unique<engine::Epochs>(db_);
    init();
  }
  Session() { init(); }
  ~Session() {
    epochs_.reset();  // stops and joins every request before the scope goes
    if (pool_) pool_->release_scope(&scope_);
  }

 private:
  void init() {
    constexpr obj::target t = obj::target::aarch64_macos;
    db_.set<CostModelIn>(t, {});
    db_.set<OptCacheSnapshot>(t, {});
    db_.set<OptLevel>(t, 0);
    // Headers are read from the disk unless the owner provides them otherwise (the daemon's file
    // table, daemon design §5.1).
    db_.set_provider<pp::FileContent>(pp::read_disk);
    // No editor buffer is open until the owner sets one (daemon design §6.1).
    db_.set_provider<pp::Buffer>(pp::no_buffer);
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
  // preprocessing configuration. The driver's file number is the unit's. An editor-view unit reads
  // its text through the view (`pp::Buffer`), so `text` is not set for it.
  void set_source(TuId tu, std::string path, std::string text, std::string object, std::string cwd = {},
                  pp::Config config = {}) {
    Bind bind(universe_);  // input values may hold handles, fingerprinted by their interners
    const pp::FileId file{tu.n};
    const bool disk = config.view == pp::View::disk;
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
    if (fresh) programs_.push_back(std::move(in));
    return ProgramId{it->second};
  }
  // The request `k` names, likewise.
  RequestId request(BuildKey k) {
    const Fp128 key = fingerprint(k);
    std::lock_guard lock(registry_m_);
    auto [it, fresh] = request_numbers_.try_emplace(key, static_cast<std::uint32_t>(requests_.size()));
    if (fresh) requests_.push_back(std::move(k));
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

  // A request's root (#139). On a pooled session it is spawned into the current epoch: a batch
  // (`apply`) restarts it where it stopped, and once `cancelled()` (polled) it stops for good and
  // this returns nothing. Otherwise it runs here. The value is copied: a later batch may replace
  // the cell's.
  template <engine::MemoQuery Q>
  std::optional<typename Q::Value> run(const typename Q::Key& k, const std::function<bool()>& cancelled = {},
                                       RunStats* stats = nullptr) {
    using V = typename Q::Value;
    if (!epochs_) {
      const engine::Stats before = db_.stats();
      std::optional<V> v{get<Q>(k)};
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
    auto ticket = epochs_->spawn<Q>(
        k, engine::Priority::interactive, [finish](const V& v) { finish([&](Wait& x) { x.value.emplace(v); }); },
        [finish](std::exception_ptr e) { finish([&](Wait& x) { x.error = std::move(e); }); });
    std::unique_lock lock(w->m);
    while (!w->cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return w->done; })) {
      if (!cancelled) continue;
      lock.unlock();
      const bool stop = cancelled();
      lock.lock();
      if (stop && !w->done) {
        ticket->cancel();
        return std::nullopt;
      }
    }
    if (stats) *stats = {ticket->restarts(), ticket->stats()};
    if (w->error) std::rethrow_exception(w->error);
    return std::move(w->value);
  }

  // Changes inputs: `write(db)` runs at quiescence. On a pooled session every running request is
  // stopped first and restarted after, keeping its finished cells (daemon design §4.1).
  template <class F>
  engine::Epochs::Applied apply(F&& write) {
    Bind bind(universe_);
    if (epochs_) return epochs_->apply(std::forward<F>(write));
    std::forward<F>(write)(db_);
    return {};
  }

  // A root request, with the session's universe bound.
  template <engine::MemoQuery Q>
  decltype(auto) get(const typename Q::Key& k) {
    Bind bind(universe_);
    return db_.get<Q>(k);
  }

  // The byte offset of a location in a source unit's main file.
  std::uint32_t byte_offset(TuId tu, ast::Loc at) {
    const pp::FileId file{tu.n};
    return pipeline::byte_offset(ins_, get<pp::Pp>(file), get<items::RegionItems>(file), at);
  }

  engine::Db& db() { return db_; }
  ast::Interners& interners() { return ins_; }

 private:
  ast::Interners ins_;
  sema::World world_{ins_};
  ValueConstants consts_{world_};
  UnitStore units_;
  Universe universe_{&ins_, &consts_, &ast::aarch64_darwin, &world_, &units_};
  engine::Db db_;
  engine::Pool* pool_ = nullptr;
  engine::Pool::Scope scope_;
  std::unique_ptr<engine::Epochs> epochs_;  // with a pool

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
  {
    const auto fronts = co_await cx.par_map<TuFront>(k.units);
    for (const FrontResult& f : fronts) {
      out.units.push_back({f.messages, f.has_errors(), std::nullopt, {}});
      all = all && !f.has_errors();
    }
  }
  if (k.until == Until::syntax) co_return out;
  std::vector<TuId> compiled;
  for (std::size_t i = 0; i < k.units.size(); ++i)
    if (!out.units[i].front_failed) compiled.push_back(k.units[i]);
  {
    const auto tus = co_await cx.par_map<Tu>(compiled);
    std::size_t j = 0;
    for (std::size_t i = 0; i < k.units.size(); ++i) {
      if (out.units[i].front_failed) continue;
      const TuValue& v = tus[j++];
      UnitBuilt& u = out.units[i];
      u.object = v.object;
      all = all && v.object.has_value();
      for (const BackError& e : v.errors) {
        std::optional<std::uint32_t> at;
        if (e.at) {
          const pp::FileId file{k.units[i].n};
          const pp::Region& region = co_await cx.fetch<pp::Pp>(file);
          const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
          at = byte_offset(*universe().in, region, items, *e.at);
        }
        u.errors.emplace_back(at, e.message);
      }
    }
  }
  if (k.until == Until::link && all) out.linked = co_await cx.fetch<Link>(k.program);
  co_return out;
}

}  // namespace ycxx::pipeline
