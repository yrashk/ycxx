// The query engine (docs/design/query-engine.md): inputs, keys and values (§A1.1–A1.3), revisions
// and durability (§A1.4), red/green validation with ordered dependency groups and early cutoff by
// fingerprint (§A1.5), entry states (§A1.6), diagnostics as values with `DiagsOf` (§A1.7), the
// memo-table layout (§A3.6), deterministic dependency recording (§A3.7) and the query-author API
// of §A10 (the core, #14); and the scheduler (#15): claims on the §A1.6 state word, waiter lists
// with the `CLOSED` sentinel, frames as coroutines on the work-stealing pool (§A3.1–A3.4), root
// requests as senders, cycles found on the wait-for graph and reported deterministically (§A2),
// cancellation by stop tokens, and revisions that advance only at quiescence (§A4).
//
// A Db without a pool runs every request on the calling thread: the same engine, with one
// worker that is the caller.
export module ycxx.engine;

export import ycxx.hash;
export import :serial;
export import :pool;
export import :arena;
import std;

export namespace ycxx::engine {

// A revision counts batches of input changes (§A1.4). Revision 0 means "never verified".
using Revision = std::uint32_t;

// How rarely an input changes (§A1.4). A derived cell's durability is the minimum over its
// dependencies, and a cell is green without visiting them while nothing of its durability or
// less durable has changed. `editor` is for unsaved buffers (daemon design §6.3), `ambient` for
// `build_time` and the optimization-cache snapshot (§4.4), below the project's own files.
enum class Durability : std::uint8_t { editor, ambient, low, medium, high };
inline constexpr std::size_t durability_levels = 5;

// What an input provider returns: the value, and the cell's durability (daemon design §5.1).
template <class V>
struct provided {
  V value;
  Durability durability;
};

// A cell is a memo-table entry: a u32 index, never reused while a frame runs (§A3.6).
using CellId = std::uint32_t;

// ---------------------------------------------------------------------------------------------
// Traits are annotations (P3394), looked up by type (§A10.1).

struct cheap_t {};
struct transparent_t {};
struct demanded_t {};
struct input {
  Durability durability;
};
inline constexpr cheap_t cheap{};
inline constexpr transparent_t transparent{};
inline constexpr demanded_t demanded{};

template <class Q, class Trait>
concept has_trait = std::meta::annotations_of_with_type(^^Q, ^^Trait).size() > 0;

// A key is a structural aggregate of handles, integers and nested structural aggregates.
consteval bool structural(std::meta::info t) {
  t = std::meta::remove_cv(t);
  if (std::meta::is_enum_type(t) || std::meta::is_integral_type(t)) return true;
  if (!std::meta::is_class_type(t) || !std::meta::is_aggregate_type(t) || !std::meta::is_trivially_copyable_type(t) ||
      !std::meta::bases_of(t, std::meta::access_context::unchecked()).empty()) {
    return false;
  }
  for (auto m : std::meta::nonstatic_data_members_of(t, std::meta::access_context::unchecked())) {
    if (!structural(std::meta::type_of(m))) return false;
  }
  return true;
}

template <class T>
concept QueryKey = structural(^^T) && std::equality_comparable<T> && sizeof(T) <= 32 && fingerprintable<T>;
template <class T>
concept QueryValue = std::movable<T> && fingerprintable<T> && serializable<T>;

class Db;
class SyncCtx;
class Ctx;
struct DiagsOf;
template <class V>
class query_task;
template <class Q>
struct q;

template <class Q>
concept QueryKind = requires {
  typename Q::Key;
  typename Q::Value;
} && QueryKey<typename Q::Key> && QueryValue<typename Q::Value>;

// An input: set from outside by `Db::set`, read by `fetch`, durability taken from its annotation.
template <class Q>
concept InputKind = QueryKind<Q> && has_trait<Q, input>;

// A cheap kind: a synchronous function of inputs and other cheap kinds (§A3.3).
template <class Q>
concept CheapQuery = QueryKind<Q> && has_trait<Q, cheap_t> && !has_trait<Q, transparent_t> &&
                     requires(SyncCtx& cx, const typename Q::Key& k) {
                       { Q::run(cx, k) } -> std::same_as<typename Q::Value>;
                     };

// The default: a memoized coroutine with the full context.
template <class Q>
concept AsyncQuery = QueryKind<Q> && !has_trait<Q, cheap_t> && !has_trait<Q, transparent_t> && !has_trait<Q, input> &&
                     requires(Ctx& cx, const typename Q::Key& k) {
                       { Q::run(cx, k) } -> std::same_as<query_task<typename Q::Value>>;
                     };

// Fetched only with `Ctx::demand`, so the request's location is recorded (§A1.7).
template <class Q>
concept DemandedQuery = AsyncQuery<Q> && has_trait<Q, demanded_t>;

// Not memoized: its fetches are recorded into the caller's dependency list (§A1.1).
template <class Q>
concept TransparentSync = has_trait<Q, transparent_t> && requires(SyncCtx& cx, const typename Q::Key& k) {
  { Q::run(cx, k) } -> std::same_as<typename Q::Value>;
};
template <class Q>
concept TransparentAsync = has_trait<Q, transparent_t> && requires(Ctx& cx, const typename Q::Key& k) {
  { Q::run(cx, k) } -> std::same_as<query_task<typename Q::Value>>;
};

// A kind with cells in the memo table.
template <class Q>
concept MemoQuery = InputKind<Q> || CheapQuery<Q> || AsyncQuery<Q>;

template <AsyncQuery Q>
class fetch_awaitable;
template <AsyncQuery... Qs>
class par_fetch_awaitable;
template <AsyncQuery Q>
class par_map_awaitable;

template <InputKind Q>
consteval Durability durability_of() {
  return std::meta::extract<input>(std::meta::annotations_of_with_type(^^Q, ^^input)[0]).durability;
}

// A kind's name and seed come from its type, so they are the same in every process.
template <class Q>
consteval std::string_view kind_name() {
  return std::define_static_string(std::meta::display_string_of(^^Q));
}
template <class Q>
consteval Fp128 kind_seed() {
  return fingerprint128(kind_name<Q>());
}

// The schema hash (§A7.1): every kind's name and the names and types of its key and value
// members, in schema order. Adding a field changes the persistent cache's key.
template <class T>
consteval void fold_schema(Hasher& h) {
  h.bytes(std::meta::display_string_of(^^T));
  if constexpr (std::is_aggregate_v<T> && std::is_class_v<T>) {
    template for (constexpr auto m : members_of<T>()) {
      h.bytes(std::meta::identifier_of(m));
      fold_schema<std::remove_cv_t<typename[:std::meta::type_of(m):]>>(h);
    }
  }
}
template <QueryKind... Qs>
struct kinds {
  static constexpr Fp128 schema_hash = [] {
    Hasher h;
    (fold_schema<Qs>(h), ...);
    (fold_schema<typename Qs::Key>(h), ...);
    (fold_schema<typename Qs::Value>(h), ...);
    return h.finish();
  }();
};

// ---------------------------------------------------------------------------------------------
// Diagnostics are values (§A1.7).

// A location: an item and an offset in it. Part B's `Loc` (ast.md §8); the item is the 32-bit
// `ItemId` handle.
struct Loc {
  std::uint32_t item = 0;
  std::uint32_t offset = 0;
  bool operator==(const Loc&) const = default;
};

// The diagnostic table belongs to the front end; the engine only carries ids.
enum class DiagId : std::uint32_t {};

// One argument: its raw value for rendering (an integer, or a handle's number) and its
// fingerprint, which is what a diagnostic's fingerprint is made of, so an argument that is a
// handle contributes its content, never its number.
struct DiagArg {
  std::uint64_t raw = 0;
  Fp128 fp;
  bool operator==(const DiagArg&) const = default;
};
void hash_into(Hasher& h, const DiagArg& a) { h.fp(a.fp); }

template <class A>
  requires ((std::integral<A> || std::is_enum_v<A>) && fingerprintable<A>)
DiagArg diag_arg(const A& a) {
  if constexpr (std::is_enum_v<A>) return {static_cast<std::uint64_t>(std::to_underlying(a)), fingerprint(a)};
  else return {static_cast<std::uint64_t>(a), fingerprint(a)};
}

struct Diagnostic {
  Loc at;
  DiagId id{};
  std::vector<DiagArg> args;
  bool operator==(const Diagnostic&) const = default;
};
void serialize_into(Sink& out, const DiagArg& a) {
  out.uint(a.raw);
  serialize(out, a.fp);
}
DiagArg deserialize_from(Source& in, std::type_identity<DiagArg>) {
  const auto raw = in.uint<std::uint64_t>();
  return {raw, deserialize<Fp128>(in)};
}

// A demand-list entry `instantiated(k, loc)` (§A1.7, §B1.4): the demanded cell is the k-th
// dependency of the cell whose value carries the list.
struct Demand {
  std::uint32_t position = 0;
  Loc at;
  bool operator==(const Demand&) const = default;
};

// A cell named as a key (for `DiagsOf`). Its fingerprint is the cell's kind and key, never its id.
struct CellRef {
  Fp128 key;
  CellId id = 0;
  bool operator==(const CellRef&) const = default;
};
void hash_into(Hasher& h, const CellRef& c) { h.fp(c.key); }

// An internal compiler error: a query's answer depends on itself (§A2). The cells are sorted by
// kind name and key fingerprint, so the report does not depend on where the cycle was entered.
class query_cycle : public std::runtime_error {
public:
  query_cycle(std::string what, std::vector<CellRef> cells) : std::runtime_error(std::move(what)), cells_(std::move(cells)) {}
  std::span<const CellRef> cells() const noexcept { return cells_; }

private:
  std::vector<CellRef> cells_;
};

// What the engine did, for tests, the harness (§A9) and the daemon's status.
struct Stats {
  std::uint64_t executed = 0;     // query bodies run
  std::uint64_t backdated = 0;    // re-executions whose fingerprint did not change (early cutoff)
  std::uint64_t validated = 0;    // cells found green by walking their dependencies
  std::uint64_t durable = 0;      // cells found green by the durability shortcut alone
  std::uint64_t hits = 0;         // fetches of a cell already verified at this revision (with
                                  // threads, approximate: a fetch that waited may count again)
  std::uint64_t recomputed = 0;   // cheap cells computed privately while another frame owned them
  std::uint64_t contended = 0;    // fetches that found a cell another frame owned, and waited
  bool operator==(const Stats&) const = default;
};

// One of `Stats`'s counts, by position.
enum class Counter : std::uint8_t { executed, backdated, validated, durable, hits, recomputed, contended };

consteval bool counters_match_stats() {
  const auto fields = members_of<Stats>();
  const auto names = std::meta::enumerators_of(^^Counter);
  if (fields.size() != names.size()) return false;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (std::meta::identifier_of(fields[i]) != std::meta::identifier_of(names[i])) return false;
  }
  return true;
}
static_assert(counters_match_stats(), "Counter names Stats's fields in order");

// `Stats` that frames on any thread count into: the Db's, a request's (daemon design §8.1), or a
// query kind's. One stripe per worker, so workers counting at once do not share a cache line.
class Counters {
public:
  static constexpr std::size_t stripes = 16;
  void add(Counter c, std::uint64_t n = 1, std::size_t stripe = 0) noexcept {
    stripes_[stripe % stripes].n[std::to_underlying(c)].fetch_add(n, std::memory_order_relaxed);
  }
  Stats stats() const noexcept {
    Stats s;
    add_to(s);
    return s;
  }
  void add_to(Stats& s) const noexcept {
    for (const Stripe& st : stripes_) {
      std::size_t i = 0;
      template for (constexpr auto m : members_of<Stats>()) s.[:m:] += st.n[i++].load(std::memory_order_relaxed);
    }
  }
  void reset() noexcept {
    for (Stripe& st : stripes_) {
      for (auto& n : st.n) n.store(0, std::memory_order_relaxed);
    }
  }

private:
  struct alignas(64) Stripe {
    std::array<std::atomic<std::uint64_t>, members_of<Stats>().size()> n{};
  };
  std::array<Stripe, stripes> stripes_{};
};

}  // namespace ycxx::engine

namespace ycxx::engine {

// The state word (§A1.6): 2 bits of state, 30 bits of owner frame, 32 bits of `verified_at`.
enum class State : std::uint8_t { empty, running, validating, done };

constexpr std::uint64_t make_word(State s, std::uint32_t owner, Revision verified) noexcept {
  return (std::uint64_t{std::to_underlying(s)} << 62) | (std::uint64_t{owner & 0x3fffffffu} << 32) | verified;
}
constexpr State state_of(std::uint64_t w) noexcept { return static_cast<State>(w >> 62); }
constexpr Revision verified_of(std::uint64_t w) noexcept { return static_cast<Revision>(w); }
constexpr std::uint32_t owner_of(std::uint64_t w) noexcept { return static_cast<std::uint32_t>(w >> 32) & 0x3fffffffu; }
constexpr bool owned(std::uint64_t w) noexcept { return state_of(w) == State::running || state_of(w) == State::validating; }

// Frame ids name owners in state words; 0 is an owner that never waits (a synchronous `cheap`
// execution) or no owner at all.
inline constexpr std::uint32_t max_frame_id = 0x3fffffffu;
inline constexpr CellId no_cell = ~CellId{0};

struct Waiter;
struct Wait;
struct Root;
class Settle;

// The waiter list's sentinel (§A3.1): a list taken by a completion is `closed`, so a late linker
// looks at the state word again instead of waiting for a completion that has already happened.
inline Waiter* closed_list() noexcept { return reinterpret_cast<Waiter*>(std::uintptr_t{1}); }

// A memo-table entry (§A3.6). The state word is the only synchronization point: the owner fills
// everything else, then publishes `Done` with a release store, and readers acquire-load the word
// before reading the rest. `durability` is also read, for the shortcut, by frames that saw an old
// `Done` while an owner validates the cell, so it is atomic.
struct Cell {
  Cell() noexcept { waiters.store(closed_list(), std::memory_order_relaxed); }

  std::atomic<std::uint64_t> word{make_word(State::empty, 0, 0)};
  std::atomic<Waiter*> waiters{nullptr};
  Revision changed_at = 0;
  std::uint32_t kind = 0;   // the kind's table in this Db
  std::uint32_t slot = 0;   // the key's and value's index in that table
  std::atomic<Durability> durability{Durability::high};
  bool input = false;
  bool cheap = false;
  bool demanded = false;
  bool has_diags = false;   // own diagnostics, or any non-demanded dependency's
  bool has_value = false;
  std::atomic<bool> observed{false};  // an input read since it was last set, reset or provided
  std::uint32_t runs = 0;
  Fp128 key_fp;             // kind seed and key
  Fp128 fp;                 // value, diagnostics, demand list and has_diags (§A1.5)
  std::vector<std::uint32_t> deps;
  std::vector<Diagnostic> diags;
  std::vector<Demand> demands;

  State state() const noexcept { return state_of(word.load(std::memory_order_acquire)); }
};

// Dependency lists are flat u32s; the first member of each group carries the high bit (§A3.6).
inline constexpr std::uint32_t group_start = 0x80000000u;

// An execution, a validation or a root request: its dependency recorder, diagnostics and demand
// list (§A3.7, §A1.7), and how it ended when it did not return a value.
struct Frame {
  Db* db = nullptr;
  Root* root = nullptr;              // whose stop token and priority this frame runs under
  CellId cell = no_cell;
  std::uint32_t id = 0;              // its owner id in state words; 0 for roots and cheap frames
  const Frame* sync_parent = nullptr;  // the synchronous chain a cheap frame runs on (§A3.3)
  bool records = true;               // a root request records nothing
  std::vector<std::uint32_t> deps;   // cell ids, groups marked with `group_start`
  std::unordered_map<CellId, std::uint32_t> recorded;  // cell -> position, past the linear-scan size
  std::vector<Diagnostic> diags;
  std::vector<Demand> demands;
  Durability durability = Durability::high;
  bool deps_have_diags = false;      // a non-demanded dependency has diagnostics
  std::coroutine_handle<> task;      // the execution's own coroutine (§A3.1)
  std::exception_ptr error;          // the internal error it ended with
  bool stopped = false;              // it was cancelled (§A4.1)

  // Records one group in argument order, a cell already recorded keeping its first position, and
  // returns each member's position.
  template <std::size_t N>
  std::array<std::uint32_t, N> record(std::span<const CellId, N> group);
  void record_dynamic(std::span<const CellId> group);
  std::uint32_t position_of(CellId c) const;
  // Folds a dependency that is verified at this revision into the frame's durability and
  // `has_diags` (what `store` needs of its dependencies).
  void fold(const Cell& c) noexcept {
    durability = std::min(durability, c.durability.load(std::memory_order_relaxed));
    deps_have_diags = deps_have_diags || (c.has_diags && !c.demanded);
  }
};

constexpr std::uint32_t linear_scan_limit = 32;

std::uint32_t Frame::position_of(CellId c) const {
  if (deps.size() <= linear_scan_limit) {
    for (std::uint32_t k = 0; k < deps.size(); ++k) {
      if ((deps[k] & ~group_start) == c) return k;
    }
    return ~0u;
  }
  const auto it = recorded.find(c);
  return it == recorded.end() ? ~0u : it->second;
}

template <std::size_t N>
std::array<std::uint32_t, N> Frame::record(std::span<const CellId, N> group) {
  std::array<std::uint32_t, N> at{};
  if (!records) return at;
  bool first = true;
  for (std::size_t k = 0; k < N; ++k) {
    const std::uint32_t seen = position_of(group[k]);
    if (seen != ~0u) {
      at[k] = seen;
      continue;
    }
    at[k] = static_cast<std::uint32_t>(deps.size());
    deps.push_back(group[k] | (first ? group_start : 0));
    first = false;
    if (deps.size() == linear_scan_limit + 1) {
      for (std::uint32_t p = 0; p < deps.size(); ++p) recorded.emplace(deps[p] & ~group_start, p);
    } else if (deps.size() > linear_scan_limit + 1) {
      recorded.emplace(group[k], at[k]);
    }
  }
  return at;
}

void Frame::record_dynamic(std::span<const CellId> group) {
  if (!records) return;
  bool first = true;
  for (CellId c : group) {
    if (position_of(c) != ~0u) continue;
    const std::array<CellId, 1> one{c};
    record(std::span<const CellId, 1>(one));
    if (!first) deps.back() &= ~group_start;  // one group: only the first member starts it
    first = false;
  }
}

// A root request (§A4.1): its stop source, its priority, and a count of what is still in flight
// on its behalf (its own result, plus every frame started for it). It completes when the count
// reaches zero, so a finished request leaves no frame behind and the engine is quiescent once
// every request has completed.
struct Root {
  Root(Db& d, CellId c, Priority p) noexcept : db(&d), cell(c), priority(p) {
    frame.db = &d;
    frame.root = this;
    frame.records = false;
  }
  Root(const Root&) = delete;
  Root& operator=(const Root&) = delete;

  void hold() noexcept { holds.fetch_add(1, std::memory_order_relaxed); }
  void release() noexcept {
    if (holds.fetch_sub(1, std::memory_order_acq_rel) == 1) complete();
  }
  // Called once, by whoever releases last; the request may be destroyed by it.
  virtual void complete() noexcept = 0;

  Db* db;
  CellId cell;
  Priority priority;
  std::inplace_stop_source stop;
  std::atomic<std::uint32_t> holds{1};
  Counters* counters = nullptr;  // what its frames do (daemon design §8.1)
  Frame frame;

protected:
  ~Root() = default;
};

// A link in one cell's waiter list.
struct Waiter {
  Waiter* next = nullptr;
  Wait* wait = nullptr;
  std::uint32_t index = 0;
};

// One suspension of one frame on the cells it found owned by others (§A3.1): one link per cell,
// a count of links not yet resolved, and the error each resolution brought. It is resumed once:
// by the resolution that brings the count to zero, or by a stop request, whichever comes first.
// Links may still sit in lists when a stop resumes it, and are resolved later, so it is
// reference-counted (one reference for the frame, one per link). The frame cannot be resumed
// while it is still linking (`suspending`): a resumption that arrives then is left `early` for
// the frame to act on itself.
struct Wait final : Task {
  enum : std::uint8_t { suspending, waiting, early, resumed };
  Wait(Db& d, Settle& s, std::size_t n, Priority p)
      : db(&d), settle(&s), prio(p), refs(static_cast<std::uint32_t>(n) + 1), pending(static_cast<std::uint32_t>(n)), links(n), errors(n) {
    run = &Wait::go;
  }

  struct OnStop {
    Wait* w;
    void operator()() const noexcept {
      w->stopped.store(true, std::memory_order_release);
      w->try_resume();
    }
  };

  // A completion took the list holding link `index`: an error for the waiter, or none (look
  // again: the cell was published, or its owner left without publishing).
  void resolve(std::uint32_t index, const std::exception_ptr& e) noexcept {
    if (e) errors[index] = e;
    if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) try_resume();
    release();
  }
  void try_resume() noexcept;
  void release() noexcept {
    if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }
  static void go(Task* t) noexcept;

  Db* db;
  Settle* settle;
  Priority prio;
  std::atomic<std::uint32_t> refs;
  std::atomic<std::uint32_t> pending;
  std::atomic<std::uint8_t> phase{suspending};
  std::atomic<bool> stopped{false};
  std::vector<Waiter> links;
  std::vector<std::exception_ptr> errors;
  std::optional<std::inplace_stop_callback<OnStop>> on_stop;
};

// `ensure_green` over a group of cells for a frame (§A1.5, §A3.1): cells verified at this
// revision are ready; a cell that needs work and is free is claimed and its frame started; a
// cell another frame owns is waited for, after the wait-for walk (§A2.2). It ends in `ready()`
// once every cell is verified at this revision, or in `failed()` when the frame was stopped or
// a cell failed. Each returns what to resume next, or nothing.
//
// When cells fail, the error reported is the first by position, as in a single-threaded run:
// an error is held until every cell before it is verified, and an earlier cell's error replaces
// it, whichever completed first or was interrupted on the way.
class Settle {
public:
  explicit Settle(Frame& f) noexcept : frame_(&f) {}

  std::coroutine_handle<> start(std::span<const CellId> cells) noexcept {
    cells_ = cells;
    return step();
  }
  // From the pool, when the wait is over: what to resume next. The frame this settles for may
  // be gone by the time that returns, so the caller resumes it, not this.
  std::coroutine_handle<> resume(Wait& w) noexcept;

protected:
  ~Settle() = default;
  virtual std::coroutine_handle<> ready() noexcept = 0;
  virtual std::coroutine_handle<> failed() noexcept = 0;
  // From an `await_suspend`: whether the awaiting coroutine `h` stays suspended. What runs next is
  // queued, never resumed on this stack, so stacks do not grow with the demand graph.
  bool suspend(std::coroutine_handle<> h, std::span<const CellId> cells) noexcept;

  Frame* frame_;

private:
  std::coroutine_handle<> step() noexcept;
  // Takes the wait's outcome and lets it go: whether it was stopped. Errors it brought are held
  // by position (`hold`).
  bool consume(Wait& w) noexcept;
  void hold(std::uint32_t at, std::exception_ptr e) noexcept {
    if (at < error_at_) {
      error_at_ = at;
      error_ = std::move(e);
    }
  }

  std::span<const CellId> cells_;
  std::vector<std::uint32_t> busy_at_;  // the position in `cells_` of each link of the wait
  std::uint32_t error_at_ = ~0u;        // the held error's position; cells from it on are not settled
  std::exception_ptr error_;
};

// A fingerprint as a hash table key: already uniform, never `std::hash` (§A5.1).
struct FpHash {
  std::size_t operator()(const Fp128& f) const noexcept { return static_cast<std::size_t>(f.lo); }
};

// A kind's part of the memo table: a sharded index from key to cell (§A3.6) and the keys and
// values by slot.
struct KindTableBase {
  virtual ~KindTableBase() = default;
  // The frame of a claimed cell, not yet started: its execution, after validating it first when
  // `validate` (an async kind).
  virtual std::coroutine_handle<> make_frame(Db& db, CellId c, std::uint32_t id, Root& root, bool validate, std::uint64_t old) = 0;
  // Runs a claimed cheap cell's query on this stack and publishes it.
  virtual void execute_sync(Db& db, CellId c, std::uint64_t restore, const Frame* chain) = 0;
  // Gives a claimed input cell without a value its provider's value, and publishes it.
  virtual void provide(Db& db, CellId c) = 0;
  std::string_view name;
  Fp128 seed;
  Counters counters;
};

template <class K>
struct key_hash {
  // In-memory: handles by number (`local_hash`), so a fetch hit reads no interner (§A3.3, #68).
  std::size_t operator()(const K& k) const noexcept { return static_cast<std::size_t>(local_hash(k).lo); }
};

inline constexpr std::size_t shard_count = 64;

template <MemoQuery Q>
struct KindTable final : KindTableBase {
  using Key = typename Q::Key;
  using Value = typename Q::Value;
  struct Slot {
    explicit Slot(const Key& k) noexcept : key(k) {}
    Key key;
    std::optional<Value> value;
  };
  struct Shard {
    std::mutex m;
    std::unordered_map<Key, CellId, key_hash<Key>> index;
  };
  std::array<Shard, shard_count> shards;
  Arena<Slot> slots;

  Shard& shard_of(const Key& k) noexcept { return shards[local_hash(k).hi % shard_count]; }
  std::coroutine_handle<> make_frame(Db& db, CellId c, std::uint32_t id, Root& root, bool validate, std::uint64_t old) override;
  void execute_sync(Db& db, CellId c, std::uint64_t restore, const Frame* chain) override;
  void provide(Db& db, CellId c) override;
  // Set at quiescence, called by any worker (daemon design §5.1).
  [[no_unique_address]] std::conditional_t<InputKind<Q>, std::function<provided<Value>(const Key&)>, std::monostate> provider;
};

std::uint32_t next_kind_slot() noexcept {
  static std::atomic<std::uint32_t> next{0};
  return next.fetch_add(1, std::memory_order_relaxed);
}
// A kind's table index in every Db. Assigned in first-use order, so it never reaches a value or
// an ordering: reports order kinds by name.
template <class Q>
std::uint32_t kind_slot() noexcept {
  static const std::uint32_t slot = next_kind_slot();
  return slot;
}
inline constexpr std::size_t max_kinds = 1024;

// The engine's coroutine for one claimed cell: validation, execution, publication or abort.
// Started from a deque; it destroys itself at the end.
struct frame_task {
  struct promise_type {
    frame_task get_return_object() noexcept { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
    std::suspend_always initial_suspend() noexcept { return {}; }
    struct final_awaiter {
      bool await_ready() noexcept { return false; }
      void await_suspend(std::coroutine_handle<promise_type> h) noexcept { h.destroy(); }
      void await_resume() noexcept {}
    };
    final_awaiter final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { std::terminate(); }  // the engine's own code
  };
  std::coroutine_handle<promise_type> h;
};

// The running coroutine's own handle.
struct own_handle {
  std::coroutine_handle<> h;
  bool await_ready() noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> c) noexcept {
    h = c;
    return false;
  }
  std::coroutine_handle<> await_resume() noexcept { return h; }
};


// The registry entry of a frame id: the cells it waits on, under the wait mutex (§A2.2).
struct Registered {
  std::vector<CellId> waits;
};

}  // namespace ycxx::engine

export namespace ycxx::engine {

template <MemoQuery Q>
class cell_sender;

// The memo table, the revision counter and the request machinery. Inputs are set at quiescence;
// requests are made with `get` (blocking) or `request` (a sender), each the root of a demand.
class Db {
public:
  // Requests run on the thread that makes them, one at a time. Any thread may make one, but never
  // two at once, and nothing else runs while one does: its stop comes from the thread that runs
  // it, and `set` and `reset_unobserved` wait for it to end (a daemon serving one request per
  // connection thread, behind its own lock, is fine).
  Db();
  // Requests run on the pool's workers. The pool outlives the Db.
  explicit Db(Pool& pool);
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
  ~Db();

  // Sets an input, and returns whether its value changed. A value with the fingerprint it already
  // has changes nothing; otherwise the first change after a request starts a new revision (one
  // revision per batch, §A1.4). The cell keeps its durability; a cell first created by `set`
  // takes its kind's, not the provider's, so set provided inputs only once they were provided.
  // It may race with `reset_unobserved` of the same input (whichever claims last wins).
  // Precondition: no request is in flight (revisions advance only at quiescence, §A4.1).
  template <InputKind Q>
  bool set(const typename Q::Key& k, typename Q::Value v);

  // An input read before anyone set it gets `fn(key)`'s value and durability, at the revision it
  // is first read, with no new revision (daemon design §5.1). `fn` runs once per cell that it
  // provides, on any worker, so it must be safe to call concurrently for different keys. A
  // throw is an internal error: the readers waiting then get it, and the next reader calls `fn`
  // again, so a missing file must be a value, not an exception. Without a provider, reading an
  // input nobody set violates a contract.
  // Precondition: no request is in flight, and no `reset_unobserved` runs.
  template <InputKind Q, class F>
    requires std::is_invocable_r_v<provided<typename Q::Value>, F&, const typename Q::Key&>
  void set_provider(F fn);

  // Changes an input in place, with no new revision and no quiescence, if nothing has read it
  // since it was last set, reset or provided; returns whether it did (daemon design §4.4). A
  // fetch that races with it either makes it fail or waits for it and reads the new value.
  // `changed_at` becomes the current revision. Callable while requests run on a Db with a pool;
  // without one, only between requests.
  template <InputKind Q>
  bool reset_unobserved(const typename Q::Key& k, typename Q::Value v);

  // A root request that blocks the calling thread until the cell is up to date, and returns its
  // value. An internal error (a cycle, an exception from query code) is rethrown.
  template <MemoQuery Q>
  decltype(auto) get(const typename Q::Key& k);

  // A root request as a sender (`cell_sender`, §A3.1): it completes with the cell's value, with
  // an internal error as `std::exception_ptr`, or stopped when its receiver's stop token asks
  // (§A4.1). It completes only once every frame it started has finished.
  // `counters`, when given, counts what the request's own frames do (daemon design §8.1); work
  // it waited for that other requests' frames did is theirs.
  template <MemoQuery Q>
  cell_sender<Q> request(const typename Q::Key& k, Priority p = Priority::background, Counters* counters = nullptr);

  // The cell of a key, created empty if it does not exist; nothing is computed or recorded.
  template <MemoQuery Q>
  CellRef cell_of(const typename Q::Key& k);

  // `diags_of` of a cell (§A1.7), as a root request.
  template <MemoQuery Q>
  const std::vector<Diagnostic>& diags_of(const typename Q::Key& k);

  Revision revision() const noexcept { return revision_.load(std::memory_order_relaxed); }
  // Frames started and not yet finished.
  std::size_t live_frames() const noexcept { return live_.load(std::memory_order_acquire); }
  Stats stats() const noexcept;
  void reset_stats() noexcept;
  // Counts per query kind as well (the daemon's debug builds, daemon design §8.1), by kind name.
  void count_per_kind(bool on) noexcept { per_kind_.store(on, std::memory_order_relaxed); }
  std::vector<std::pair<std::string_view, Stats>> stats_per_kind() const;
  Pool* pool() const noexcept { return pool_; }
  // How many times a cell's query has run; 0 for a cell that does not exist.
  template <MemoQuery Q>
  std::uint32_t runs(const typename Q::Key& k) const;
  // The revision at which a cell's value last changed; 0 for a cell that does not exist.
  template <MemoQuery Q>
  Revision changed_at(const typename Q::Key& k) const;
  // A cell's dependency groups in program order (§A3.7); empty for a cell that does not exist.
  template <MemoQuery Q>
  std::vector<std::vector<CellRef>> dependencies(const typename Q::Key& k) const;
  // A cell's demand list (§A1.7).
  template <MemoQuery Q>
  std::vector<Demand> demands(const typename Q::Key& k) const;

  // The memo-table comparison of incremental == cold (§A9): every cell `cold` executed and still
  // holds valid at its last revision, found here by its key's fingerprint and brought up to date,
  // has the same fingerprint and dependency list, and none of it executes; an input has the same
  // fingerprint. A cell `cold` has and this Db never computed is reported, not computed.
  // Preconditions: both Dbs are quiescent and in this process, and their keys mean the same:
  // keys holding interner handles need one set of interners shared by both, until handles are
  // fingerprinted by content (§A7.1, #68).
  struct TableCheck {
    std::uint64_t checked = 0;
    std::uint64_t executed = 0;           // executions here while checking: 0 when it holds
    std::vector<std::string> mismatches;  // "kind key-fingerprint: what differs", at most 20
    bool ok() const noexcept { return executed == 0 && mismatches.empty(); }
  };
  TableCheck verify_table(const Db& cold);

private:
  friend class SyncCtx;
  friend class Ctx;
  friend struct DiagsOf;
  friend class Settle;
  friend struct Wait;
  friend struct Root;
  friend struct Request;
  template <MemoQuery Q>
  friend struct KindTable;
  template <AsyncQuery Q>
  friend class fetch_awaitable;
  template <AsyncQuery... Qs>
  friend class par_fetch_awaitable;
  template <AsyncQuery Q>
  friend class par_map_awaitable;
  friend class cell_awaitable;
  friend class stop_awaitable;
  template <MemoQuery Q>
  friend class cell_sender;

  enum class Settled : bool { ready, busy };

  template <MemoQuery Q>
  KindTable<Q>& table() const;
  template <MemoQuery Q>
  std::optional<CellId> find_cell(const typename Q::Key& k) const;
  template <MemoQuery Q>
  CellId cell_for(const typename Q::Key& k);
  template <MemoQuery Q>
  const typename Q::Key& key_of(CellId c) const;
  template <MemoQuery Q>
  const typename Q::Value& value_of(CellId c) const;
  CellRef ref(CellId c) const { return {cells_[c].key_fp, c}; }
  std::string_view kind_name_of(CellId c) const { return tables_[cells_[c].kind].load(std::memory_order_acquire)->name; }

  // Verified at this revision (an input always is): the fetch-hit test (§A3.3).
  bool is_current(CellId c) const noexcept {
    const Cell& e = cells_[c];
    const std::uint64_t w = e.word.load(std::memory_order_acquire);
    return state_of(w) == State::done && (e.input || verified_of(w) == revision());
  }
  Durability durability_now(const Cell& e) const noexcept { return e.durability.load(std::memory_order_relaxed); }
  bool durable(const Cell& e, std::uint64_t w) const noexcept {
    return verified_of(w) >= last_changed_[std::to_underlying(durability_now(e))].load(std::memory_order_relaxed);
  }

  // One step of `ensure_green` for a frame (§A1.5): ready, or busy because it is now claimed for a
  // new frame (pushed onto `children`) or owned by another frame.
  Settled try_settle(CellId c, Frame& requester, std::vector<std::coroutine_handle<>>& children);
  // `ensure_green` of an input or cheap cell on this stack (§A3.3): false if another frame owns it.
  // Re-entering a cell of the synchronous chain is a cycle (thrown as `query_cycle`).
  bool ensure_sync(CellId c, const Frame* chain);
  bool ensure_input(CellId c, const Frame* chain);
  template <InputKind Q>
  void provide(CellId c);
  void validate_sync(CellId c, std::uint64_t old, const Frame* chain);
  template <CheapQuery Q>
  typename Q::Value fetch_cheap(CellId c, Frame& f);
  template <MemoQuery Q>
  void execute_sync(CellId c, std::uint64_t restore, const Frame* chain);
  template <AsyncQuery Q>
  static frame_task run_frame(Db& db, CellId c, std::uint32_t id, Root& root, bool validate, std::uint64_t old);

  std::coroutine_handle<> start_frame(CellId c, std::uint32_t id, Root& root, bool validate, std::uint64_t old);
  void finish(Frame& f) noexcept;
  // After a successful claim: the waiter list becomes linkable (§A3.1).
  void reopen(Cell& e) noexcept;
  template <MemoQuery Q>
  void store(CellId c, typename Q::Value v, Frame& f);
  // Publishes `Done` at this revision and wakes every waiter.
  void publish(CellId c) noexcept;
  // Leaves a claimed cell without publishing: the word goes back to `restore` (Empty, or the old
  // Done) and every waiter gets `error`, or looks again when there is none (§A1.6, §A3.1).
  void abort(CellId c, std::uint64_t restore, const std::exception_ptr& error) noexcept;
  void wake(Waiter* list, const std::exception_ptr& error) noexcept;

  // The wait-for walk (§A2.2), under `wait_mutex_`.
  std::optional<std::vector<CellId>> find_cycle(const Frame& me, std::span<const CellId> busy) const;
  std::exception_ptr cycle_error(std::vector<CellId> cells) const;
  [[noreturn]] void sync_cycle(const Frame* chain, CellId c) const;

  std::uint32_t take_id();
  void give_id(std::uint32_t id) noexcept;

  void push(Work w, Priority p) noexcept;
  // Runs queued work on this thread until there is none (no pool).
  void drain() noexcept;
  void chaos_point() noexcept {
    if (pool_) pool_->chaos_point();
  }
  // Counts into the Db's stripe, the request's counters and, when on, the cell's kind.
  void count(Counter what, const Root* root, CellId c, std::uint64_t n = 1) noexcept;
  void observe() noexcept { observed_.store(true, std::memory_order_relaxed); }

  Arena<Cell> cells_;
  mutable std::array<std::atomic<KindTableBase*>, max_kinds> tables_{};
  std::array<std::atomic<Revision>, durability_levels> last_changed_{};
  std::atomic<Revision> revision_{1};
  std::atomic<bool> observed_{false};
  std::atomic<std::size_t> live_{0};
  Pool* pool_ = nullptr;
  std::vector<Work> local_;  // the queue when there is no pool
  std::atomic<std::thread::id> driver_{};  // without a pool, the thread running a request now
  std::mutex wait_mutex_;
  Arena<Registered> registry_;
  std::mutex ids_mutex_;
  std::vector<std::uint32_t> free_ids_;
  Counters stats_;
  std::atomic<bool> per_kind_{false};
};

// ---------------------------------------------------------------------------------------------
// The context handles (§A10.2): the only way a query observes the world.

class SyncCtx {
public:
  template <CheapQuery Q>
  typename Q::Value fetch(const typename Q::Key& k) {
    const CellId c = db_->cell_for<Q>(k);
    record_one(c);
    return db_->fetch_cheap<Q>(c, *frame_);
  }
  template <InputKind Q>
  const typename Q::Value& fetch(const typename Q::Key& k) {
    const CellId c = db_->cell_for<Q>(k);
    record_one(c);
    while (!db_->ensure_sync(c, frame_)) std::this_thread::yield();  // a provider or reset holds it, briefly
    frame_->fold(db_->cells_[c]);
    return db_->value_of<Q>(c);
  }
  template <TransparentSync Q>
  typename Q::Value fetch(const typename Q::Key& k) {
    return Q::run(*this, k);
  }

  // Reports a diagnostic that holds in every context that can reach this cell (§A1.7).
  template <class... A>
  void diag(Loc at, DiagId id, const A&... args) {
    frame_->diags.push_back({at, id, {diag_arg(args)...}});
  }

  CellId self() const noexcept { return frame_->cell; }

protected:
  SyncCtx(Db& db, Frame& f) noexcept : db_(&db), frame_(&f) {}
  std::uint32_t record_one(CellId c) {
    const std::array<CellId, 1> one{c};
    return frame_->record(std::span<const CellId, 1>(one))[0];
  }

  Db* db_;
  Frame* frame_;

  friend class Db;
};

// A query's coroutine type (§A3.1). Lazily started. The engine starts it by symmetric transfer
// and it transfers back to whoever awaited it when it returns. A fetch that is stopped or fails
// never resumes it: the engine destroys it from the top (§A4.1).
template <class V>
class [[nodiscard]] query_task {
public:
  struct promise_type {
    std::optional<V> value;
    std::exception_ptr error;
    std::coroutine_handle<> continuation;
    query_task get_return_object() noexcept { return query_task(std::coroutine_handle<promise_type>::from_promise(*this)); }
    std::suspend_always initial_suspend() noexcept { return {}; }
    struct final_awaiter {
      bool await_ready() noexcept { return false; }
      std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        const std::coroutine_handle<> c = h.promise().continuation;
        return c ? c : std::noop_coroutine();
      }
      void await_resume() noexcept {}
    };
    final_awaiter final_suspend() noexcept { return {}; }
    template <class U>
      requires std::convertible_to<U, V>
    void return_value(U&& v) {
      value.emplace(std::forward<U>(v));
    }
    void unhandled_exception() noexcept { error = std::current_exception(); }
  };

  query_task(query_task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
  query_task& operator=(query_task&&) = delete;
  ~query_task() {
    if (h_) h_.destroy();
  }

  // Runs the body to completion on this stack, for code outside the engine whose every await
  // completes inline (the interpreter's host, until evaluation runs as a query). An exception
  // escaping the body is rethrown.
  V run() && {
    h_.resume();
    contract_assert(h_.done());  // it awaited something that suspended
    return await_resume();
  }

  // Awaiting a task (a transparent helper) runs it on the awaiting query's behalf.
  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
    h_.promise().continuation = h;
    return h_;
  }
  V await_resume() {
    if (h_.promise().error) std::rethrow_exception(h_.promise().error);
    return std::move(*h_.promise().value);
  }

private:
  friend class Db;
  explicit query_task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}

  std::coroutine_handle<promise_type> h_;
};

// `co_await cx.fetch<Q>(k)`: the hit path in `await_ready`, `ensure_green` in `await_suspend`
// (§A3.3).
template <AsyncQuery Q>
class fetch_awaitable : Settle {
public:
  bool await_ready() {
    const std::uint32_t at = record_self();
    if (loc_) frame_->demands.push_back({at, *loc_});
    if (!db_->is_current(cell_)) return false;
    db_->count(Counter::hits, frame_->root, cell_);
    return true;
  }
  bool await_suspend(std::coroutine_handle<> h) noexcept {
    h_ = h;
    return suspend(h, std::span<const CellId>(&cell_, 1));
  }
  const typename Q::Value& await_resume() const {
    frame_->fold(db_->cells_[cell_]);
    return db_->value_of<Q>(cell_);
  }

private:
  friend class Ctx;
  fetch_awaitable(Db& db, Frame& f, CellId c, std::optional<Loc> loc) noexcept : Settle(f), db_(&db), cell_(c), loc_(loc) {}
  std::uint32_t record_self() {
    const std::array<CellId, 1> one{cell_};
    return frame_->record(std::span<const CellId, 1>(one))[0];
  }
  std::coroutine_handle<> ready() noexcept override { return h_; }
  std::coroutine_handle<> failed() noexcept override { return frame_->task; }

  Db* db_;
  CellId cell_;
  std::optional<Loc> loc_;
  std::coroutine_handle<> h_;
};

// One member of a `par_fetch`.
template <class Q>
struct q {
  typename Q::Key key;
};

template <AsyncQuery... Qs>
class par_fetch_awaitable : Settle {
public:
  bool await_ready() {
    frame_->record(std::span<const CellId, sizeof...(Qs)>(cells_));
    for (CellId c : cells_) {
      if (!db_->is_current(c)) return false;
    }
    for (CellId c : cells_) db_->count(Counter::hits, frame_->root, c);
    return true;
  }
  bool await_suspend(std::coroutine_handle<> h) noexcept {
    h_ = h;
    return suspend(h, cells_);
  }
  std::tuple<const typename Qs::Value&...> await_resume() const {
    for (CellId c : cells_) frame_->fold(db_->cells_[c]);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return std::tuple<const typename Qs::Value&...>(db_->value_of<Qs>(cells_[I])...);
    }(std::index_sequence_for<Qs...>());
  }

private:
  friend class Ctx;
  par_fetch_awaitable(Db& db, Frame& f, std::array<CellId, sizeof...(Qs)> cells) noexcept : Settle(f), db_(&db), cells_(cells) {}
  std::coroutine_handle<> ready() noexcept override { return h_; }
  std::coroutine_handle<> failed() noexcept override { return frame_->task; }

  Db* db_;
  std::array<CellId, sizeof...(Qs)> cells_;
  std::coroutine_handle<> h_;
};

template <AsyncQuery Q>
class par_map_awaitable : Settle {
public:
  bool await_ready() {
    frame_->record_dynamic(cells_);
    for (CellId c : cells_) {
      if (!db_->is_current(c)) return false;
    }
    for (CellId c : cells_) db_->count(Counter::hits, frame_->root, c);
    return true;
  }
  bool await_suspend(std::coroutine_handle<> h) noexcept {
    h_ = h;
    return suspend(h, cells_);
  }
  std::vector<std::reference_wrapper<const typename Q::Value>> await_resume() const {
    std::vector<std::reference_wrapper<const typename Q::Value>> out;
    out.reserve(cells_.size());
    for (CellId c : cells_) {
      frame_->fold(db_->cells_[c]);
      out.push_back(std::cref(db_->value_of<Q>(c)));
    }
    return out;
  }

private:
  friend class Ctx;
  par_map_awaitable(Db& db, Frame& f, std::vector<CellId> cells) noexcept : Settle(f), db_(&db), cells_(std::move(cells)) {}
  std::coroutine_handle<> ready() noexcept override { return h_; }
  std::coroutine_handle<> failed() noexcept override { return frame_->task; }

  Db* db_;
  std::vector<CellId> cells_;
  std::coroutine_handle<> h_;
};

}  // namespace ycxx::engine

namespace ycxx::engine {

// Any cell by id, for the engine's own queries (`DiagsOf`): recorded and brought up to date,
// without a value.
class cell_awaitable : Settle {
public:
  cell_awaitable(Db& db, Frame& f, CellId c) noexcept : Settle(f), db_(&db), cell_(c) {}
  bool await_ready() {
    const std::array<CellId, 1> one{cell_};
    frame_->record(std::span<const CellId, 1>(one));
    return db_->is_current(cell_);
  }
  bool await_suspend(std::coroutine_handle<> h) noexcept {
    h_ = h;
    return suspend(h, std::span<const CellId>(&cell_, 1));
  }
  void await_resume() const noexcept { frame_->fold(db_->cells_[cell_]); }

private:
  std::coroutine_handle<> ready() noexcept override { return h_; }
  std::coroutine_handle<> failed() noexcept override { return frame_->task; }

  Db* db_;
  CellId cell_;
  std::coroutine_handle<> h_;
};

// `co_await cx.stop_point()` (§A3.5): completes stopped, never resuming the query, when its
// request was cancelled.
class stop_awaitable {
public:
  explicit stop_awaitable(Frame& f) noexcept : frame_(&f) {}
  bool await_ready() const noexcept { return !frame_->root->stop.stop_requested(); }
  bool await_suspend(std::coroutine_handle<>) noexcept;
  void await_resume() const noexcept {}

private:
  Frame* frame_;
};

}  // namespace ycxx::engine

export namespace ycxx::engine {

class Ctx : public SyncCtx {
public:
  using SyncCtx::fetch;

  template <AsyncQuery Q>
    requires (!DemandedQuery<Q>)
  fetch_awaitable<Q> fetch(const typename Q::Key& k) {
    return {*db_, *frame_, db_->cell_for<Q>(k), std::nullopt};
  }
  template <TransparentAsync Q>
  query_task<typename Q::Value> fetch(const typename Q::Key& k) {
    return Q::run(*this, k);
  }
  // Fetches a demanded cell and appends `instantiated(k, at)` to this cell's demand list (§A1.7).
  template <DemandedQuery Q>
  fetch_awaitable<Q> demand(const typename Q::Key& k, Loc at) {
    return {*db_, *frame_, db_->cell_for<Q>(k), at};
  }
  // One dependency group: members recorded in argument order before any runs (§A3.7); values
  // returned positionally.
  template <AsyncQuery... Qs>
    requires (!DemandedQuery<Qs> && ...)
  par_fetch_awaitable<Qs...> par_fetch(q<Qs>... qs) {
    return {*db_, *frame_, {db_->cell_for<Qs>(qs.key)...}};
  }
  template <AsyncQuery Q, std::ranges::input_range R>
    requires (!DemandedQuery<Q> && std::convertible_to<std::ranges::range_reference_t<R>, typename Q::Key>)
  par_map_awaitable<Q> par_map(R&& keys) {
    std::vector<CellId> cells;
    for (auto&& k : keys) cells.push_back(db_->cell_for<Q>(k));
    return {*db_, *frame_, std::move(cells)};
  }
  // A key for `DiagsOf`; records nothing.
  template <MemoQuery Q>
  CellRef cell_of(const typename Q::Key& k) {
    return db_->ref(db_->cell_for<Q>(k));
  }
  // The request's stop token (§A4.1), for leaves that poll.
  std::inplace_stop_token stop_token() const noexcept { return frame_->root->stop.get_token(); }
  // A budget checkpoint of a long-running leaf (§A3.5): completes stopped if the request was
  // cancelled, so the query never resumes and stores nothing.
  stop_awaitable stop_point() noexcept { return stop_awaitable(*frame_); }

private:
  friend class Db;
  friend struct DiagsOf;
  Ctx(Db& db, Frame& f) noexcept : SyncCtx(db, f) {}
  cell_awaitable fetch_cell(CellId c) noexcept { return {*db_, *frame_, c}; }
};

// `diags_of(cell)` (§A1.7): the cell's own diagnostics, then each dependency's in program order,
// not descending into demanded kinds, pruned by `has_diags`.
//
// It depends on the cell, on every dependency of the cell, and on `DiagsOf` of each dependency
// that has diagnostics. The cell's dependency list changes only when it re-executes, which
// happens only when one of the dependencies `DiagsOf` recorded changed after the cell was last
// verified, hence after `DiagsOf` was (the cell is fetched first). So `DiagsOf` re-runs whenever
// the list may have changed, even if the cell itself was backdated. A dependency whose subgraph
// gains or loses diagnostics changes its fingerprint through `has_diags`, so pruning never hides
// a change.
struct DiagsOf {
  using Key = CellRef;
  using Value = std::vector<Diagnostic>;
  static query_task<Value> run(Ctx& cx, CellRef target);
};

}  // namespace ycxx::engine

namespace ycxx::engine {

// A root request's settling and completion, shared by `get` and `cell_sender`.
struct Request : Root, Settle {
  Request(Db& d, CellId c, Priority p) noexcept : Root(d, c, p), Settle(Root::frame) {}

  // Starts settling the cell. Without a pool the work runs here, before this returns.
  void begin() noexcept {
    Db& d = *db;
    // At start, not when the sender was made: a `set` between the two must not leave the revision
    // this request reads at unmarked (review of #95, P0).
    d.observe();
    // Without a pool, the request owns the Db until it has drained: one request at a time.
    std::thread::id before{};
    if (!d.pool_) {
      before = d.driver_.exchange(std::this_thread::get_id(), std::memory_order_relaxed);
      const bool one_at_a_time = before == std::thread::id{} || before == std::this_thread::get_id();
      contract_assert(one_at_a_time);
    }
    (void)start(std::span<const CellId>(&cell, 1));
    d.drain();
    if (!d.pool_) d.driver_.store(before, std::memory_order_relaxed);
  }

  std::exception_ptr error;
  bool stopped = false;

protected:
  ~Request() = default;

private:
  std::coroutine_handle<> ready() noexcept override {
    release();
    return {};
  }
  std::coroutine_handle<> failed() noexcept override {
    error = frame.error;
    stopped = frame.stopped;
    // Like `when_all`: the frames still running for this request stop too.
    stop.request_stop();
    release();
    return {};
  }
};

}  // namespace ycxx::engine

export namespace ycxx::engine {

// The future of a memo cell as a sender (§A3.1): `start` is `ensure_green` from a root.
template <MemoQuery Q>
class cell_sender {
public:
  using sender_concept = std::execution::sender_tag;
  template <class Self, class... Env>
  static consteval auto get_completion_signatures() {
    return std::execution::completion_signatures<std::execution::set_value_t(const typename Q::Value&),
                                                 std::execution::set_error_t(std::exception_ptr), std::execution::set_stopped_t()>();
  }

  template <class R>
  class op : Request {
  public:
    using operation_state_concept = std::execution::operation_state_tag;
    op(Db& db, CellId c, Priority p, Counters* counters, R r) : Request(db, c, p), rcvr_(std::move(r)) {
      this->counters = counters;
    }

    void start() & noexcept {
      auto token = std::get_stop_token(std::execution::get_env(rcvr_));
      if (token.stop_requested()) {
        std::execution::set_stopped(std::move(rcvr_));
        return;
      }
      if constexpr (!std::unstoppable_token<token_t>) forward_.emplace(token, Forward{this});
      begin();
    }

  private:
    using token_t = std::stop_token_of_t<std::execution::env_of_t<R>>;
    struct Forward {
      Root* root;
      void operator()() const noexcept { root->stop.request_stop(); }
    };

    void complete() noexcept override {
      forward_.reset();
      if (stopped) std::execution::set_stopped(std::move(rcvr_));
      else if (error) std::execution::set_error(std::move(rcvr_), error);
      else std::execution::set_value(std::move(rcvr_), db->value_of<Q>(cell));
    }

    R rcvr_;
    std::optional<std::stop_callback_for_t<token_t, Forward>> forward_;
  };

  template <class R>
  op<std::decay_t<R>> connect(R&& r) const {
    return {*db_, cell_, prio_, counters_, std::forward<R>(r)};
  }

private:
  friend class Db;
  cell_sender(Db& db, CellId c, Priority p, Counters* counters) noexcept : db_(&db), cell_(c), prio_(p), counters_(counters) {}
  Db* db_;
  CellId cell_;
  Priority prio_;
  Counters* counters_;
};

// Root requests grouped in revision epochs (§A4.1), for the daemon's edit strand (#83): requests
// are spawned into the current epoch's `counting_scope`; `apply` stops them, joins the scope,
// writes the inputs at quiescence, opens the next epoch and respawns into it every request the
// stop interrupted, so a client sees one answer (daemon design §4.1). Calls are serialized, so a
// request spawned while `apply` runs waits for the new epoch: the sequential strand and its gate.
class Epochs {
public:
  // A spawned request. `cancel` stops it for good: it completes stopped and is not respawned.
  class Ticket : public std::enable_shared_from_this<Ticket> {
  public:
    void cancel() noexcept { client_.request_stop(); }
    // What its runs did, all restarts included (daemon design §8.1).
    Stats stats() const noexcept { return counters_.stats(); }
    // How many times an epoch's stop interrupted it and it was respawned.
    std::size_t restarts() const noexcept { return restarts_.load(std::memory_order_relaxed); }

  private:
    friend class Epochs;
    std::inplace_stop_source client_;
    Counters counters_;
    std::uint64_t executed_at_restart_ = 0;
    std::atomic<std::size_t> restarts_{0};
    std::function<void()> launch_;  // spawns it into the current epoch
  };

  // What `apply` did with the requests it stopped: how many it respawned, and how many of those
  // had executed (stored) no cell since their previous start (the daemon's progress quantum
  // grows on those, daemon design §4.2).
  struct Applied {
    std::size_t respawned = 0;
    std::size_t starved = 0;
  };

  explicit Epochs(Db& db) : db_(&db), scope_(std::make_unique<std::execution::counting_scope>()) {}
  Epochs(const Epochs&) = delete;
  Epochs& operator=(const Epochs&) = delete;
  ~Epochs() {
    std::lock_guard lock(m_);
    finish(true);
    std::lock_guard again(stopped_m_);
    stopped_.clear();
  }

  // A request in the current epoch: `on_value(const Value&)` when it completes with a value,
  // `on_error(std::exception_ptr)` on an internal error; when an epoch's stop interrupts it, it
  // runs again in the next epoch. Neither callback may call back into this object.
  template <MemoQuery Q, class F, class G>
  std::shared_ptr<Ticket> spawn(const typename Q::Key& k, Priority p, F on_value, G on_error) {
    namespace ex = std::execution;
    auto ticket = std::make_shared<Ticket>();
    auto callbacks = std::make_shared<std::pair<F, G>>(std::move(on_value), std::move(on_error));
    ticket->launch_ = [this, t = ticket.get(), k, p, callbacks] {
      std::shared_ptr<Ticket> self = t->shared_from_this();
      ex::spawn(db_->request<Q>(k, p, &t->counters_) |
                    ex::then([callbacks](const typename Q::Value& v) mutable { callbacks->first(v); }) |
                    ex::upon_error([callbacks](std::exception_ptr e) mutable noexcept { callbacks->second(std::move(e)); }) |
                    ex::upon_stopped([this, self]() noexcept { interrupted(self); }),
                scope_->get_token(), ex::prop(std::get_stop_token, t->client_.get_token()));
    };
    std::lock_guard lock(m_);
    ticket->launch_();
    return ticket;
  }

  // Waits for every request of the current epoch to complete, and opens the next one.
  void join() {
    std::lock_guard lock(m_);
    finish(false);
    scope_ = std::make_unique<std::execution::counting_scope>();
  }

  // Stops and joins the current epoch, runs `write(db)` at quiescence (it sets inputs), opens
  // the next epoch and respawns the requests the stop interrupted.
  // If `write` throws, the next epoch is still opened and the interrupted requests respawned
  // (on whatever inputs were written), and the exception propagates.
  template <class F>
  Applied apply(F&& write) {
    std::lock_guard lock(m_);
    finish(true);
    scope_ = std::make_unique<std::execution::counting_scope>();
    std::exception_ptr failure;
    try {
      std::forward<F>(write)(*db_);
    } catch (...) {
      failure = std::current_exception();
    }
    std::vector<std::shared_ptr<Ticket>> again;
    {
      std::lock_guard l(stopped_m_);
      again.swap(stopped_);
    }
    Applied a;
    for (const auto& t : again) {
      const std::uint64_t executed = t->counters_.stats().executed;
      ++a.respawned;
      if (executed == t->executed_at_restart_) ++a.starved;
      t->executed_at_restart_ = executed;
      t->restarts_.fetch_add(1, std::memory_order_relaxed);
      t->launch_();
    }
    if (failure) std::rethrow_exception(failure);
    return a;
  }

private:
  void finish(bool stop) {
    if (stop) scope_->request_stop();
    scope_->close();
    std::this_thread::sync_wait(scope_->join());
    contract_assert(db_->live_frames() == 0);
  }
  // A request completed stopped: by its client, for good, or by the epoch, to run again.
  void interrupted(const std::shared_ptr<Ticket>& t) noexcept {
    if (t->client_.stop_requested()) return;
    std::lock_guard lock(stopped_m_);
    stopped_.push_back(t);
  }

  Db* db_;
  std::mutex m_;
  std::unique_ptr<std::execution::counting_scope> scope_;
  std::mutex stopped_m_;
  std::vector<std::shared_ptr<Ticket>> stopped_;
};

}  // namespace ycxx::engine

namespace ycxx::engine {

// ---------------------------------------------------------------------------------------------
// Waiting.

void Wait::try_resume() noexcept {
  std::uint8_t p = phase.load(std::memory_order_acquire);
  while (true) {
    if (p == suspending) {
      if (phase.compare_exchange_weak(p, early, std::memory_order_acq_rel, std::memory_order_acquire)) return;
    } else if (p == waiting) {
      if (phase.compare_exchange_weak(p, resumed, std::memory_order_acq_rel, std::memory_order_acquire)) {
        db->push(this, prio);
        return;
      }
    } else {
      return;
    }
  }
}

void Wait::go(Task* t) noexcept {
  Wait* w = static_cast<Wait*>(t);
  if (const std::coroutine_handle<> next = w->settle->resume(*w)) next.resume();
}

bool Settle::consume(Wait& w) noexcept {
  Db& db = *frame_->db;
  if (frame_->id != 0) {
    std::lock_guard lock(db.wait_mutex_);
    db.registry_[frame_->id].waits.clear();
  }
  w.on_stop.reset();
  const bool all = w.pending.load(std::memory_order_acquire) == 0;
  if (all) {
    for (std::size_t i = 0; i < w.errors.size(); ++i) {
      if (w.errors[i]) hold(busy_at_[i], w.errors[i]);
    }
  }
  w.release();
  return !all;
}

std::coroutine_handle<> Settle::resume(Wait& w) noexcept {
  if (consume(w)) {
    frame_->stopped = true;
    return failed();
  }
  return step();
}

bool Settle::suspend(std::coroutine_handle<> h, std::span<const CellId> cells) noexcept {
  Db& db = *frame_->db;
  const Priority prio = frame_->root->priority;
  const std::coroutine_handle<> next = start(cells);
  if (next == h) return false;
  if (next) db.push(next, prio);
  return true;
}

std::coroutine_handle<> Settle::step() noexcept {
  Db& db = *frame_->db;
  const Priority prio = frame_->root->priority;
  std::vector<std::coroutine_handle<>> children;
  std::vector<CellId> busy;
  while (true) {
    if (frame_->root->stop.stop_requested()) {
      frame_->stopped = true;
      return failed();
    }
    db.chaos_point();
    children.clear();
    busy.clear();
    busy_at_.clear();
    // Only the cells before a held error are settled: it is reported once they are verified.
    const auto limit = static_cast<std::uint32_t>(std::min<std::size_t>(cells_.size(), error_at_));
    for (std::uint32_t i = 0; i < limit; ++i) {
      try {
        if (db.try_settle(cells_[i], *frame_, children) == Db::Settled::busy) {
          busy.push_back(cells_[i]);
          busy_at_.push_back(i);
        }
      } catch (...) {
        // A cheap cell failed on this stack (an exception from its query, or a cycle).
        hold(i, std::current_exception());
        break;
      }
    }
    // Claimed cells always get their frames started: something may already wait on them. Pushed
    // in reverse, so this worker takes the first next.
    const auto start_children = [&] {
      for (auto it = children.rbegin(); it != children.rend(); ++it) db.push(*it, prio);
    };
    std::exception_ptr error;
    if (busy.empty()) {
      if (!error_) return ready();
      error = error_;
    }
    if (!error && frame_->id != 0) {
      std::lock_guard lock(db.wait_mutex_);
      if (auto cycle = db.find_cycle(*frame_, busy)) error = db.cycle_error(std::move(*cycle));
      else db.registry_[frame_->id].waits.assign(busy.begin(), busy.end());
    }
    if (error) {
      start_children();
      frame_->error = error;
      return failed();
    }
    Wait* w = new Wait(db, *this, busy.size(), prio);
    std::size_t unlinked = 0;
    for (std::uint32_t i = 0; i < busy.size(); ++i) {
      Waiter& l = w->links[i];
      l.wait = w;
      l.index = i;
      Cell& e = db.cells_[busy[i]];
      Waiter* head = e.waiters.load(std::memory_order_acquire);
      bool linked = false;
      while (head != closed_list()) {
        l.next = head;
        if (e.waiters.compare_exchange_weak(head, &l, std::memory_order_acq_rel, std::memory_order_acquire)) {
          linked = true;
          break;
        }
      }
      // The list was taken (the owner finished or left) or not yet reopened (a claim in
      // progress): resolved at once, and the cell is looked at again after this round.
      if (!linked) {
        ++unlinked;
        w->resolve(i, nullptr);
      }
    }
    const std::inplace_stop_token token = frame_->root->stop.get_token();
    w->on_stop.emplace(token, Wait::OnStop{w});
    start_children();
    std::uint8_t expected = Wait::suspending;
    if (w->phase.compare_exchange_strong(expected, Wait::waiting, std::memory_order_acq_rel, std::memory_order_acquire)) return {};
    // Everything resolved (or a stop arrived) while linking: carry on here.
    if (consume(*w)) {
      frame_->stopped = true;
      return failed();
    }
    if (unlinked == busy.size()) std::this_thread::yield();  // a claim between its two CASes (§A3.1)
  }
}

bool stop_awaitable::await_suspend(std::coroutine_handle<>) noexcept {
  frame_->stopped = true;
  frame_->db->push(frame_->task, frame_->root->priority);
  return true;
}

void Db::wake(Waiter* list, const std::exception_ptr& error) noexcept {
  while (list != nullptr && list != closed_list()) {
    Waiter* next = list->next;
    list->wait->resolve(list->index, error);
    list = next;
  }
}

void Db::reopen(Cell& e) noexcept {
  Waiter* expected = closed_list();
  e.waiters.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed);
}

void Db::publish(CellId c) noexcept {
  Cell& e = cells_[c];
  e.word.store(make_word(State::done, 0, revision()), std::memory_order_release);
  wake(e.waiters.exchange(closed_list(), std::memory_order_acq_rel), nullptr);
}

void Db::abort(CellId c, std::uint64_t restore, const std::exception_ptr& error) noexcept {
  Cell& e = cells_[c];
  // The list first: a claimant arriving after the word is restored finds it closed and reopens
  // it, so it never inherits waiters of the frame that left.
  Waiter* list = e.waiters.exchange(closed_list(), std::memory_order_acq_rel);
  e.word.store(restore, std::memory_order_release);
  wake(list, error);
}

// ---------------------------------------------------------------------------------------------
// Claims.

Db::Settled Db::try_settle(CellId c, Frame& requester, std::vector<std::coroutine_handle<>>& children) {
  Cell& e = cells_[c];
  if (e.input || e.cheap) return ensure_sync(c, &requester) ? Settled::ready : Settled::busy;
  const Revision r = revision();
  std::uint64_t w = e.word.load(std::memory_order_acquire);
  while (true) {
    switch (state_of(w)) {
      case State::done: {
        if (verified_of(w) == r) {
          count(Counter::hits, requester.root, c);
          return Settled::ready;
        }
        if (durable(e, w)) {
          if (e.word.compare_exchange_weak(w, make_word(State::done, 0, r), std::memory_order_acq_rel, std::memory_order_acquire)) {
            count(Counter::durable, requester.root, c);
            return Settled::ready;
          }
          continue;
        }
        const std::uint32_t id = take_id();
        if (!e.word.compare_exchange_strong(w, make_word(State::validating, id, verified_of(w)), std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
          give_id(id);
          continue;
        }
        reopen(e);
        children.push_back(start_frame(c, id, *requester.root, true, w));
        return Settled::busy;
      }
      case State::empty: {
        const std::uint32_t id = take_id();
        if (!e.word.compare_exchange_strong(w, make_word(State::running, id, 0), std::memory_order_acq_rel, std::memory_order_acquire)) {
          give_id(id);
          continue;
        }
        reopen(e);
        children.push_back(start_frame(c, id, *requester.root, false, w));
        return Settled::busy;
      }
      case State::running:
      case State::validating:
        count(Counter::contended, requester.root, c);
        return Settled::busy;
    }
  }
}

// An input (§A1.1): marked observed, then read once no provider or reset holds it (daemon design
// §4.4). The mark and the claim of `reset_unobserved` are a Dekker pair (a sequentially
// consistent store, then load, on each side), so a reset either sees the mark and gives up, or
// claimed the cell before this load and is waited for. One without a value is provided here, at
// this revision (§5.1).
bool Db::ensure_input(CellId c, const Frame* chain) {
  Cell& e = cells_[c];
  const Root* root = chain ? chain->root : nullptr;
  // A seq_cst load that finds the mark already set orders this read after it as well as a store
  // would, without every reader writing the cell's line.
  if (!e.observed.load(std::memory_order_seq_cst)) e.observed.store(true, std::memory_order_seq_cst);
  std::uint64_t w = e.word.load(std::memory_order_seq_cst);
  while (true) {
    switch (state_of(w)) {
      case State::done:
        count(Counter::hits, root, c);
        return true;
      case State::empty:
        if (!e.word.compare_exchange_strong(w, make_word(State::running, 0, 0), std::memory_order_seq_cst)) continue;
        reopen(e);
        tables_[e.kind].load(std::memory_order_acquire)->provide(*this, c);
        return true;
      case State::running:
      case State::validating:
        return false;
    }
  }
}

bool Db::ensure_sync(CellId c, const Frame* chain) {
  Cell& e = cells_[c];
  if (e.input) return ensure_input(c, chain);
  const Root* root = chain ? chain->root : nullptr;
  const Revision r = revision();
  std::uint64_t w = e.word.load(std::memory_order_acquire);
  while (true) {
    switch (state_of(w)) {
      case State::done:
        if (verified_of(w) == r) {
          count(Counter::hits, root, c);
          return true;
        }
        if (durable(e, w)) {
          if (e.word.compare_exchange_weak(w, make_word(State::done, 0, r), std::memory_order_acq_rel, std::memory_order_acquire)) {
            count(Counter::durable, root, c);
            return true;
          }
          continue;
        }
        if (!e.word.compare_exchange_strong(w, make_word(State::validating, 0, verified_of(w)), std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
          continue;
        }
        reopen(e);
        validate_sync(c, w, chain);
        return true;
      case State::empty:
        if (!e.word.compare_exchange_strong(w, make_word(State::running, 0, 0), std::memory_order_acq_rel, std::memory_order_acquire)) {
          continue;
        }
        reopen(e);
        tables_[e.kind].load(std::memory_order_acquire)->execute_sync(*this, c, make_word(State::empty, 0, 0), chain);
        return true;
      case State::running:
      case State::validating:
        for (const Frame* f = chain; f; f = f->sync_parent) {
          if (f->cell == c) sync_cycle(chain, c);
        }
        return false;
    }
  }
}

// Validation of a claimed cheap cell on this stack (§A1.5). Its dependencies are inputs and cheap
// cells; one that another chain owns is being computed synchronously and never waits, so this
// yields until it is done rather than suspending.
void Db::validate_sync(CellId c, std::uint64_t old, const Frame* chain) {
  Cell& e = cells_[c];
  Frame vf;
  vf.db = this;
  vf.root = chain ? chain->root : nullptr;
  vf.cell = c;
  vf.sync_parent = chain;
  const Revision since = verified_of(old);
  bool changed = false;
  Durability d = Durability::high;
  try {
    for (std::size_t k = 0; k < e.deps.size() && !changed;) {
      do {
        const CellId dep = e.deps[k] & ~group_start;
        while (!ensure_sync(dep, &vf)) std::this_thread::yield();
        changed = changed || cells_[dep].changed_at > since;
        d = std::min(d, durability_now(cells_[dep]));
        ++k;
      } while (k < e.deps.size() && !(e.deps[k] & group_start));
    }
  } catch (...) {
    abort(c, old, std::current_exception());
    throw;
  }
  if (!changed) {
    e.durability.store(d, std::memory_order_relaxed);
    count(Counter::validated, vf.root, c);
    publish(c);
    return;
  }
  e.word.store(make_word(State::running, 0, since), std::memory_order_release);
  tables_[e.kind].load(std::memory_order_acquire)->execute_sync(*this, c, make_word(State::done, 0, since), chain);
}

template <MemoQuery Q>
void Db::execute_sync(CellId c, std::uint64_t restore, const Frame* chain) {
  if constexpr (CheapQuery<Q>) {
    Frame f;
    f.db = this;
    f.root = chain ? chain->root : nullptr;
    f.cell = c;
    f.sync_parent = chain;
    std::optional<typename Q::Value> v;
    try {
      SyncCtx cx(*this, f);
      v.emplace(Q::run(cx, key_of<Q>(c)));
    } catch (...) {
      abort(c, restore, std::current_exception());
      throw;
    }
    store<Q>(c, std::move(*v), f);
    publish(c);
  } else {
    contract_assert(false);  // only cheap kinds run synchronously
  }
}

// A cheap fetch (§A3.3): the cell's own value when it is, or can be brought, up to date here; a
// private computation when another frame is computing it, never published.
template <CheapQuery Q>
typename Q::Value Db::fetch_cheap(CellId c, Frame& f) {
  if (ensure_sync(c, &f)) {
    f.fold(cells_[c]);
    return value_of<Q>(c);
  }
  Frame pf;
  pf.db = this;
  pf.root = f.root;
  pf.cell = c;
  pf.sync_parent = &f;
  SyncCtx cx(*this, pf);
  typename Q::Value v = Q::run(cx, key_of<Q>(c));
  const bool has_diags = !pf.diags.empty() || pf.deps_have_diags;
  f.durability = std::min(f.durability, pf.durability);
  f.deps_have_diags = f.deps_have_diags || has_diags;
  count(Counter::recomputed, f.root, c);
  const Cell& e = cells_[c];
  const std::uint64_t w = e.word.load(std::memory_order_acquire);
  if (state_of(w) == State::done && verified_of(w) == revision()) {
    Hasher h;
    hash_append(h, v);
    hash_append(h, pf.diags);
    hash_append(h, pf.demands);
    hash_append(h, has_diags);
    contract_assert(h.finish() == e.fp);  // the owner published the same value
  }
  return v;
}

std::coroutine_handle<> Db::start_frame(CellId c, std::uint32_t id, Root& root, bool validate, std::uint64_t old) {
  root.hold();
  live_.fetch_add(1, std::memory_order_relaxed);
  return tables_[cells_[c].kind].load(std::memory_order_acquire)->make_frame(*this, c, id, root, validate, old);
}

// The end of a frame: its id goes back, the live count drops, and last its request's hold,
// which may complete the request (after that nothing of the Db may be touched).
void Db::finish(Frame& f) noexcept {
  give_id(f.id);
  Root* root = f.root;
  if (live_.fetch_sub(1, std::memory_order_acq_rel) == 1) live_.notify_all();
  root->release();
}

// A validating frame's wait for one dependency group (§A1.5); the frame looks at its outcome.
class settle_group : Settle {
public:
  settle_group(Frame& f, std::span<const CellId> cells) noexcept : Settle(f), cells_(cells) {}
  bool await_ready() const noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> h) noexcept { return suspend(h, cells_); }
  void await_resume() const noexcept {}

private:
  std::coroutine_handle<> ready() noexcept override { return frame_->task; }
  std::coroutine_handle<> failed() noexcept override { return frame_->task; }
  std::span<const CellId> cells_;
};

// Starts a query's body by symmetric transfer; its return transfers back.
template <class V>
struct run_body {
  std::coroutine_handle<typename query_task<V>::promise_type> h;
  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> me) noexcept {
    h.promise().continuation = me;
    return h;
  }
  void await_resume() const noexcept {}
};

template <AsyncQuery Q>
frame_task Db::run_frame(Db& db, CellId c, std::uint32_t id, Root& root, bool validate, std::uint64_t old) {
  Frame f;
  f.db = &db;
  f.root = &root;
  f.cell = c;
  f.id = id;
  f.task = co_await own_handle{};
  Cell& e = db.cells_[c];
  // The word to restore if the frame leaves without publishing: Empty, or the old Done (§A1.6).
  std::uint64_t restore = make_word(State::empty, 0, 0);
  if (validate) {
    restore = old;
    const Revision since = verified_of(old);
    bool changed = false;
    Durability d = Durability::high;
    std::vector<CellId> group;
    // Groups in program order; every member of a group, with no short-circuit (§A1.5). The
    // dependency list does not change while this frame owns the cell.
    for (std::size_t k = 0; k < e.deps.size() && !changed;) {
      group.clear();
      do {
        group.push_back(e.deps[k] & ~group_start);
        ++k;
      } while (k < e.deps.size() && !(e.deps[k] & group_start));
      co_await settle_group(f, group);
      if (f.error || f.stopped) {
        db.abort(c, restore, f.error);
        db.finish(f);
        co_return;
      }
      for (CellId m : group) {
        changed = changed || db.cells_[m].changed_at > since;
        d = std::min(d, db.durability_now(db.cells_[m]));
      }
    }
    if (!changed) {
      // Durability is not part of the fingerprint: a dependency backdated after re-executing may
      // have lowered its own, and the shortcut must not trust this cell's old one. Every
      // dependency was visited, so the minimum is exact; a cell green by the shortcut needs no
      // update, since lowering a dependency's durability takes a re-execution somewhere below it,
      // caused by an input change that also moved `last_changed_` at this cell's level.
      e.durability.store(d, std::memory_order_relaxed);
      db.count(Counter::validated, &root, c);
      db.publish(c);
      db.finish(f);
      co_return;
    }
    // Red: re-execute. Dependencies after the changed group may not be fetched by the new run.
    e.word.store(make_word(State::running, id, since), std::memory_order_release);
    restore = make_word(State::done, 0, since);
  }
  {
    Ctx cx(db, f);
    // `run` that throws before its coroutine exists is an internal error like any other.
    std::optional<query_task<typename Q::Value>> body;
    try {
      body.emplace(Q::run(cx, db.key_of<Q>(c)));
    } catch (...) {
      f.error = std::current_exception();
    }
    if (body) {
      co_await run_body<typename Q::Value>{body->h_};
      // Resumed by the body's return, or by a fetch that was stopped or failed (which leaves the
      // body suspended; destroying `body` destroys it and every helper it awaits).
      if (!f.error && !f.stopped) {
        if (body->h_.promise().error) {
          f.error = body->h_.promise().error;
        } else {
          db.store<Q>(c, std::move(*body->h_.promise().value), f);
          db.publish(c);
          db.finish(f);
          co_return;
        }
      }
    }
  }
  db.abort(c, restore, f.error);
  db.finish(f);
}

template <MemoQuery Q>
std::coroutine_handle<> KindTable<Q>::make_frame(Db& db, CellId c, std::uint32_t id, Root& root, bool validate, std::uint64_t old) {
  if constexpr (AsyncQuery<Q>) {
    return Db::run_frame<Q>(db, c, id, root, validate, old).h;
  } else {
    contract_assert(false);  // inputs and cheap kinds have no frames
    return {};
  }
}

template <MemoQuery Q>
void KindTable<Q>::execute_sync(Db& db, CellId c, std::uint64_t restore, const Frame* chain) {
  db.execute_sync<Q>(c, restore, chain);
}

template <MemoQuery Q>
void KindTable<Q>::provide(Db& db, CellId c) {
  if constexpr (InputKind<Q>) db.provide<Q>(c);
  else contract_assert(false);  // only inputs are provided
}

template <MemoQuery Q>
void Db::store(CellId c, typename Q::Value v, Frame& f) {
  Cell& e = cells_[c];
  contract_assert(e.state() == State::running);
  // `has_diags` is part of the fingerprint: when a dependency gains or loses diagnostics, this
  // cell's value may be unchanged, but `DiagsOf` above it must stop pruning here or start again.
  const bool has_diags = !f.diags.empty() || f.deps_have_diags;
  Hasher h;
  hash_append(h, v);
  hash_append(h, f.diags);
  hash_append(h, f.demands);
  hash_append(h, has_diags);
  const Fp128 fp = h.finish();
  ++e.runs;
  count(Counter::executed, f.root, c);
  if (e.has_value && fp == e.fp) {
    count(Counter::backdated, f.root, c);  // early cutoff: `changed_at` stays
  } else {
    table<Q>().slots[e.slot].value = std::move(v);
    e.fp = fp;
    e.changed_at = revision();
    e.has_value = true;
  }
  e.deps = std::move(f.deps);
  e.diags = std::move(f.diags);
  e.demands = std::move(f.demands);
  e.durability.store(f.durability, std::memory_order_relaxed);
  e.has_diags = has_diags;
  contract_assert(e.changed_at <= revision());
}

// ---------------------------------------------------------------------------------------------
// Cycles (§A2).

std::optional<std::vector<CellId>> Db::find_cycle(const Frame& me, std::span<const CellId> busy) const {
  struct Node {
    CellId cell;
    std::uint32_t parent;
  };
  std::vector<Node> nodes;
  std::vector<std::uint32_t> seen;
  for (CellId c : busy) nodes.push_back({c, ~0u});
  for (std::uint32_t i = 0; i < nodes.size(); ++i) {
    const std::uint64_t w = cells_[nodes[i].cell].word.load(std::memory_order_acquire);
    if (!owned(w)) continue;
    const std::uint32_t owner = owner_of(w);
    if (owner == 0) continue;  // a synchronous owner never waits
    if (owner == me.id) {
      std::vector<CellId> cells;
      for (std::uint32_t j = i; j != ~0u; j = nodes[j].parent) cells.push_back(nodes[j].cell);
      return cells;
    }
    if (std::ranges::contains(seen, owner)) continue;
    seen.push_back(owner);
    for (CellId next : registry_[owner].waits) nodes.push_back({next, i});
  }
  return std::nullopt;
}

// The report (§A2.3): the cells on the cycle sorted by kind name and key fingerprint, so it does
// not depend on where the cycle was entered or which frame found it.
std::exception_ptr Db::cycle_error(std::vector<CellId> ids) const {
  std::vector<CellRef> cells;
  for (CellId c : ids) cells.push_back(ref(c));
  std::ranges::sort(cells, [&](const CellRef& a, const CellRef& b) {
    return std::tuple(kind_name_of(a.id), a.key) < std::tuple(kind_name_of(b.id), b.key);
  });
  std::string what = "internal error: query cycle";
  for (const CellRef& r : cells) what += std::format("\n  {} {:016x}{:016x}", kind_name_of(r.id), r.key.hi, r.key.lo);
  return std::make_exception_ptr(query_cycle(std::move(what), std::move(cells)));
}

void Db::sync_cycle(const Frame* chain, CellId c) const {
  std::vector<CellId> cells;
  for (const Frame* f = chain; f; f = f->sync_parent) {
    cells.push_back(f->cell);
    if (f->cell == c) break;
  }
  std::rethrow_exception(cycle_error(std::move(cells)));
}

// ---------------------------------------------------------------------------------------------
// Frame ids, the queue and statistics.

std::uint32_t Db::take_id() {
  {
    std::lock_guard lock(ids_mutex_);
    if (!free_ids_.empty()) {
      const std::uint32_t id = free_ids_.back();
      free_ids_.pop_back();
      return id;
    }
  }
  const std::uint32_t id = registry_.emplace();
  contract_assert(id <= max_frame_id);
  return id;
}

void Db::give_id(std::uint32_t id) noexcept {
  if (id == 0) return;
  std::lock_guard lock(ids_mutex_);
  free_ids_.push_back(id);
}

void Db::push(Work w, Priority p) noexcept {
  if (pool_) {
    pool_->push(w, p);
    return;
  }
  // A stop requested from another thread would push here while the request's thread drains.
  contract_assert(std::this_thread::get_id() == driver_.load(std::memory_order_relaxed));
  local_.push_back(w);
}

void Db::drain() noexcept {
  if (pool_) return;
  while (!local_.empty()) {
    const Work w = local_.back();
    local_.pop_back();
    w();
  }
}

void Db::count(Counter what, const Root* root, CellId c, std::uint64_t n) noexcept {
  const std::size_t stripe = pool_ ? pool_->worker_index().value_or(pool_->size()) : 0;
  stats_.add(what, n, stripe);
  if (root && root->counters) root->counters->add(what, n, stripe);
  if (per_kind_.load(std::memory_order_relaxed)) tables_[cells_[c].kind].load(std::memory_order_acquire)->counters.add(what, n, stripe);
}

Db::Db() { (void)registry_.emplace(); }  // id 0 is never an owner that waits

Db::Db(Pool& pool) : Db() { pool_ = &pool; }

Db::~Db() {
  contract_assert(live_frames() == 0);
  for (auto& t : tables_) delete t.load(std::memory_order_relaxed);
}

Stats Db::stats() const noexcept {
  Stats s;
  stats_.add_to(s);
  return s;
}

void Db::reset_stats() noexcept {
  stats_.reset();
  for (const auto& t : tables_) {
    if (KindTableBase* k = t.load(std::memory_order_acquire)) k->counters.reset();
  }
}

std::vector<std::pair<std::string_view, Stats>> Db::stats_per_kind() const {
  std::vector<std::pair<std::string_view, Stats>> out;
  for (const auto& t : tables_) {
    if (const KindTableBase* k = t.load(std::memory_order_acquire)) out.emplace_back(k->name, k->counters.stats());
  }
  std::ranges::sort(out, {}, &std::pair<std::string_view, Stats>::first);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Tables.

template <MemoQuery Q>
KindTable<Q>& Db::table() const {
  const std::uint32_t s = kind_slot<Q>();
  contract_assert(s < max_kinds);
  KindTableBase* t = tables_[s].load(std::memory_order_acquire);
  if (!t) {
    auto fresh = std::make_unique<KindTable<Q>>();
    fresh->name = kind_name<Q>();
    fresh->seed = kind_seed<Q>();
    if (tables_[s].compare_exchange_strong(t, fresh.get(), std::memory_order_acq_rel, std::memory_order_acquire)) t = fresh.release();
  }
  return static_cast<KindTable<Q>&>(*t);
}

template <MemoQuery Q>
std::optional<CellId> Db::find_cell(const typename Q::Key& k) const {
  KindTable<Q>& t = table<Q>();
  auto& shard = t.shard_of(k);
  std::lock_guard lock(shard.m);
  if (const auto it = shard.index.find(k); it != shard.index.end()) return it->second;
  return std::nullopt;
}

template <MemoQuery Q>
CellId Db::cell_for(const typename Q::Key& k) {
  KindTable<Q>& t = table<Q>();
  auto& shard = t.shard_of(k);
  std::lock_guard lock(shard.m);
  if (const auto it = shard.index.find(k); it != shard.index.end()) return it->second;
  const std::uint32_t slot = t.slots.emplace(k);
  const CellId c = cells_.emplace();
  contract_assert(c < group_start);
  Cell& cell = cells_[c];
  cell.kind = kind_slot<Q>();
  cell.slot = slot;
  cell.input = InputKind<Q>;
  cell.cheap = CheapQuery<Q>;
  if constexpr (InputKind<Q>) cell.durability.store(durability_of<Q>(), std::memory_order_relaxed);
  if constexpr (AsyncQuery<Q>) cell.demanded = DemandedQuery<Q>;
  Hasher h;
  h.fp(t.seed);
  hash_append(h, k);
  cell.key_fp = h.finish();
  shard.index.emplace(k, c);
  return c;
}

template <MemoQuery Q>
const typename Q::Key& Db::key_of(CellId c) const {
  return table<Q>().slots[cells_[c].slot].key;
}

template <MemoQuery Q>
const typename Q::Value& Db::value_of(CellId c) const {
  const Cell& cell = cells_[c];
  contract_assert(cell.has_value);
  return *table<Q>().slots[cell.slot].value;
}

// ---------------------------------------------------------------------------------------------
// Queries of the engine's own.

query_task<std::vector<Diagnostic>> DiagsOf::run(Ctx& cx, CellRef target) {
  Db& db = *cx.db_;
  co_await cx.fetch_cell(target.id);
  const Cell& t = db.cells_[target.id];
  std::vector<Diagnostic> out = t.diags;
  // The target is verified at this revision, so its list does not change while this runs.
  for (std::uint32_t d : t.deps) {
    const CellId dep = d & ~group_start;
    co_await cx.fetch_cell(dep);
    const Cell& c = db.cells_[dep];
    if (c.has_diags && !c.demanded) {
      const auto& sub = co_await cx.fetch<DiagsOf>(db.ref(dep));
      out.append_range(sub);
    }
  }
  co_return out;
}

// ---------------------------------------------------------------------------------------------
// Requests and inputs.

// `get`'s request: the blocked thread waits on a flag it shares with the completer, so the
// request can go away as soon as the flag is set.
struct Blocking final : Request {
  using Request::Request;
  std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
  void complete() noexcept override {
    const auto d = done;
    d->store(true, std::memory_order_release);
    d->notify_all();
  }
};

Db::TableCheck Db::verify_table(const Db& cold) {
  contract_assert(live_frames() == 0 && cold.live_frames() == 0);
  TableCheck r;
  const auto note = [&](CellId c, std::string_view what) {
    if (r.mismatches.size() < 20) {
      r.mismatches.push_back(std::format("{} {:016x}{:016x}: {}", cold.kind_name_of(c), cold.cells_[c].key_fp.hi, cold.cells_[c].key_fp.lo, what));
    }
  };
  std::unordered_map<Fp128, CellId, FpHash> ids;
  std::unordered_set<Fp128, FpHash> shared;  // key fingerprints of two cells here
  for (CellId c = 0; c < cells_.size(); ++c) {
    if (!ids.emplace(cells_[c].key_fp, c).second) shared.insert(cells_[c].key_fp);
  }
  const auto keys_of = [](const Db& db, const Cell& e) {
    std::vector<Fp128> out;
    for (std::uint32_t d : e.deps) out.push_back(db.cells_[d & ~group_start].key_fp);
    return out;
  };
  const std::uint64_t before = stats().executed;
  for (CellId c = 0; c < cold.cells_.size(); ++c) {
    const Cell& want = cold.cells_[c];
    if (want.input ? !want.has_value : want.runs == 0) continue;
    const std::uint64_t w = want.word.load(std::memory_order_acquire);
    if (!want.input && !(state_of(w) == State::done && verified_of(w) == cold.revision())) continue;  // stale in `cold`
    ++r.checked;
    if (shared.contains(want.key_fp)) {
      note(c, "two cells here share its key fingerprint");
      continue;
    }
    const auto it = ids.find(want.key_fp);
    if (it == ids.end()) {
      note(c, "never computed here");
      continue;
    }
    const CellId here = it->second;
    if (!want.input) {
      Blocking b(*this, here, Priority::background);
      const auto done = b.done;
      b.begin();
      done->wait(false, std::memory_order_acquire);
      if (b.error) {
        note(c, "an internal error bringing it up to date");
        continue;
      }
    }
    const Cell& got = cells_[here];
    if (!got.has_value || got.fp != want.fp) note(c, "a different value (fingerprint)");
    else if (!want.input && keys_of(*this, got) != keys_of(cold, want)) note(c, "a different dependency list");
  }
  r.executed = stats().executed - before;
  return r;
}

template <InputKind Q>
bool Db::set(const typename Q::Key& k, typename Q::Value v) {
  // BOOTSTRAP: contracts: GCC 16.2 drops `pre` on a function template instantiated through an
  // import (STATUS, "Bootstrap workarounds"); an assertion in the body is kept.
  contract_assert(live_frames() == 0);
  const CellId c = cell_for<Q>(k);
  Cell& e = cells_[c];
  // Claimed like a reset, which may run on another thread (daemon design §4.4: admission resets
  // `build_time` while the strand writes a batch).
  std::uint64_t w = e.word.load(std::memory_order_seq_cst);
  while (owned(w) || !e.word.compare_exchange_weak(w, make_word(State::running, 0, verified_of(w)), std::memory_order_seq_cst)) {
    if (owned(w)) {
      std::this_thread::yield();
      w = e.word.load(std::memory_order_seq_cst);
    }
  }
  const Fp128 fp = fingerprint(v);
  if (e.has_value && fp == e.fp) {
    e.word.store(w, std::memory_order_release);
    return false;
  }
  e.observed.store(false, std::memory_order_relaxed);
  Revision r = revision();
  if (observed_.exchange(false, std::memory_order_relaxed)) revision_.store(++r, std::memory_order_relaxed);
  table<Q>().slots[e.slot].value = std::move(v);
  e.fp = fp;
  e.has_value = true;
  e.changed_at = r;
  // Every durability at or below the input's has now changed.
  for (std::size_t d = 0; d <= std::to_underlying(durability_now(e)); ++d) last_changed_[d].store(r, std::memory_order_relaxed);
  e.word.store(make_word(State::done, 0, r), std::memory_order_release);
  return true;
}

template <InputKind Q, class F>
  requires std::is_invocable_r_v<provided<typename Q::Value>, F&, const typename Q::Key&>
void Db::set_provider(F fn) {
  contract_assert(live_frames() == 0);
  table<Q>().provider = std::move(fn);
}

template <InputKind Q>
void Db::provide(CellId c) {
  Cell& e = cells_[c];
  const auto& provider = table<Q>().provider;
  contract_assert(static_cast<bool>(provider));  // an input is set or provided before it is read
  std::optional<provided<typename Q::Value>> got;
  try {
    got.emplace(provider(key_of<Q>(c)));
  } catch (...) {
    abort(c, make_word(State::empty, 0, 0), std::current_exception());
    throw;
  }
  // The first value of the cell: nothing depended on it, so no `last_changed` moves (§5.1).
  e.fp = fingerprint(got->value);
  table<Q>().slots[e.slot].value = std::move(got->value);
  e.durability.store(got->durability, std::memory_order_relaxed);
  e.changed_at = revision();
  e.has_value = true;
  publish(c);
}

template <InputKind Q>
bool Db::reset_unobserved(const typename Q::Key& k, typename Q::Value v) {
  const CellId c = cell_for<Q>(k);
  Cell& e = cells_[c];
  std::uint64_t w = e.word.load(std::memory_order_seq_cst);
  if (owned(w)) return false;  // a provider or another reset holds it
  if (!e.word.compare_exchange_strong(w, make_word(State::running, 0, verified_of(w)), std::memory_order_seq_cst)) return false;
  reopen(e);
  if (e.observed.load(std::memory_order_seq_cst)) {
    abort(c, w, nullptr);
    return false;
  }
  const Fp128 fp = fingerprint(v);
  if (!e.has_value || fp != e.fp) {
    table<Q>().slots[e.slot].value = std::move(v);
    e.fp = fp;
    e.has_value = true;
    e.changed_at = revision();
  }
  publish(c);
  return true;
}

template <MemoQuery Q>
decltype(auto) Db::get(const typename Q::Key& k) {
  observe();
  const CellId c = cell_for<Q>(k);
  if constexpr (InputKind<Q>) {
    // Marked and read as every input read is, so a racing reset either fails or is waited for.
    while (!ensure_input(c, nullptr)) std::this_thread::yield();
  } else if (is_current(c)) {
    count(Counter::hits, nullptr, c);
  } else {
    Blocking b(*this, c, Priority::background);
    const auto done = b.done;
    b.begin();
    done->wait(false, std::memory_order_acquire);
    if (b.error) std::rethrow_exception(b.error);
  }
  if constexpr (CheapQuery<Q>) return typename Q::Value(value_of<Q>(c));
  else return value_of<Q>(c);
}

template <MemoQuery Q>
cell_sender<Q> Db::request(const typename Q::Key& k, Priority p, Counters* counters) {
  observe();
  return {*this, cell_for<Q>(k), p, counters};
}

template <MemoQuery Q>
CellRef Db::cell_of(const typename Q::Key& k) {
  return ref(cell_for<Q>(k));
}

template <MemoQuery Q>
const std::vector<Diagnostic>& Db::diags_of(const typename Q::Key& k) {
  return get<DiagsOf>(cell_of<Q>(k));
}

template <MemoQuery Q>
std::uint32_t Db::runs(const typename Q::Key& k) const {
  const auto c = find_cell<Q>(k);
  return c ? cells_[*c].runs : 0;
}

template <MemoQuery Q>
Revision Db::changed_at(const typename Q::Key& k) const {
  const auto c = find_cell<Q>(k);
  return c ? cells_[*c].changed_at : 0;
}

template <MemoQuery Q>
std::vector<std::vector<CellRef>> Db::dependencies(const typename Q::Key& k) const {
  std::vector<std::vector<CellRef>> out;
  const auto c = find_cell<Q>(k);
  if (!c) return out;
  for (std::uint32_t d : cells_[*c].deps) {
    if (d & group_start) out.emplace_back();
    out.back().push_back(ref(d & ~group_start));
  }
  return out;
}

template <MemoQuery Q>
std::vector<Demand> Db::demands(const typename Q::Key& k) const {
  const auto c = find_cell<Q>(k);
  if (!c) return {};
  return cells_[*c].demands;
}

}  // namespace ycxx::engine
