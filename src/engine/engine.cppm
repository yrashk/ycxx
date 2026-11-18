// The query engine's core (docs/design/query-engine.md, #14): inputs, keys and values (§A1.1–A1.3),
// revisions and durability (§A1.4), red/green validation with ordered dependency groups and early
// cutoff by fingerprint (§A1.5), entry states (§A1.6), diagnostics as values with `DiagsOf`
// (§A1.7), the memo-table layout (§A3.6), deterministic dependency recording (§A3.7), and the
// query-author API of §A10.
//
// Evaluation is single-threaded and synchronous for now. A query is still written as the design
// has it, a coroutine returning `query_task<V>` that `co_await`s `cx.fetch<Q>(k)`, but every
// await completes inline: a miss refreshes the child cell on the current stack. The scheduler
// (#15) replaces exactly that: `fetch_awaitable::await_suspend` and `Db::refresh` become claims,
// `cell_sender`s and the pool, and a cycle is delivered through `set_error` instead of the
// `query_cycle` exception used here. The state word already has the §A1.6 layout and is
// updated with the orderings the concurrent engine needs.
export module ycxx.engine;

export import ycxx.hash;
export import :serial;
import std;

export namespace ycxx::engine {

// A revision counts batches of input changes (§A1.4). Revision 0 means "never verified".
using Revision = std::uint32_t;

// How rarely an input changes (§A1.4). A derived cell's durability is the minimum over its
// dependencies, and a cell is green without visiting them while nothing of its durability or
// less durable has changed.
enum class Durability : std::uint8_t { low, medium, high };
inline constexpr std::size_t durability_levels = 3;

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
  std::uint64_t hits = 0;         // fetches of a cell already verified at this revision
  bool operator==(const Stats&) const = default;
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

// A memo-table entry (§A3.6).
struct Cell {
  std::atomic<std::uint64_t> word{make_word(State::empty, 0, 0)};
  Revision changed_at = 0;
  std::uint32_t kind = 0;   // the kind's table in this Db
  std::uint32_t slot = 0;   // the key's and value's index in that table
  Durability durability = Durability::high;
  bool input = false;
  bool demanded = false;
  bool has_diags = false;   // own diagnostics, or any non-demanded dependency's
  bool has_value = false;
  std::uint32_t runs = 0;
  Fp128 key_fp;             // kind seed and key
  Fp128 fp;                 // value, diagnostics, demand list and has_diags (§A1.5)
  std::uint32_t deps_begin = 0, deps_len = 0;
  std::uint32_t diags_begin = 0, diags_len = 0;
  std::uint32_t demands_begin = 0, demands_len = 0;

  State state() const noexcept { return state_of(word.load(std::memory_order_acquire)); }
  Revision verified_at() const noexcept { return verified_of(word.load(std::memory_order_acquire)); }
};

// Dependency lists are flat u32s; the first member of each group carries the high bit (§A3.6).
inline constexpr std::uint32_t group_start = 0x80000000u;

// The executing query's dependency recorder, diagnostics and demand list (§A3.7, §A1.7).
struct Frame {
  CellId cell = 0;
  bool root = false;               // an outside request: records nothing
  std::vector<std::uint32_t> deps; // cell ids, groups marked with `group_start`
  std::unordered_map<CellId, std::uint32_t> recorded;  // cell -> position, past the linear-scan size
  std::vector<Diagnostic> diags;
  std::vector<Demand> demands;
  Durability durability = Durability::high;

  // Records one group in argument order, a cell already recorded keeping its first position, and
  // returns each member's position.
  template <std::size_t N>
  std::array<std::uint32_t, N> record(std::span<const CellId, N> group);
  std::uint32_t position_of(CellId c) const;
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
  if (root) return at;
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

// A kind's part of the memo table. Keys and values live in deques, so references to them stay
// valid while the table grows; the index is the in-memory hash map of §A3.6 (sharded and
// concurrent with #15).
struct KindTableBase {
  virtual ~KindTableBase() = default;
  virtual void execute(Db& db, CellId c) = 0;
  std::string_view name;
  Fp128 seed;
};

template <class K>
struct key_hash {
  std::size_t operator()(const K& k) const noexcept { return static_cast<std::size_t>(fingerprint(k).lo); }
};

template <MemoQuery Q>
struct KindTable final : KindTableBase {
  using Key = typename Q::Key;
  using Value = typename Q::Value;
  std::unordered_map<Key, CellId, key_hash<Key>> index;
  std::deque<Key> keys;
  std::deque<std::optional<Value>> values;
  void execute(Db& db, CellId c) override;
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

}  // namespace ycxx::engine

export namespace ycxx::engine {

// The memo table and the revision counter. Inputs are set between requests; requests are made
// with `get`, which is the root of a demand.
class Db {
public:
  Db() = default;
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Sets an input. A value with the fingerprint it already has changes nothing; otherwise the
  // first change after a request starts a new revision (one revision per batch, §A1.4).
  // Precondition: no query is running (revisions advance only at quiescence, §A4.1).
  template <InputKind Q>
  void set(const typename Q::Key& k, typename Q::Value v);

  // A root request: brings the cell up to date and returns its value.
  template <MemoQuery Q>
  decltype(auto) get(const typename Q::Key& k);

  // The cell of a key, created empty if it does not exist; nothing is computed or recorded.
  template <MemoQuery Q>
  CellRef cell_of(const typename Q::Key& k);

  // `diags_of` of a cell (§A1.7), as a root request.
  template <MemoQuery Q>
  const std::vector<Diagnostic>& diags_of(const typename Q::Key& k);

  Revision revision() const noexcept { return revision_; }
  std::size_t live_frames() const noexcept { return active_.size(); }
  const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = {}; }
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

private:
  friend class SyncCtx;
  friend class Ctx;
  friend struct DiagsOf;
  template <MemoQuery Q>
  friend struct KindTable;
  template <AsyncQuery Q>
  friend class fetch_awaitable;
  template <AsyncQuery... Qs>
  friend class par_fetch_awaitable;
  template <AsyncQuery Q>
  friend class par_map_awaitable;

  template <MemoQuery Q>
  KindTable<Q>& table();
  template <MemoQuery Q>
  const KindTable<Q>* find_table() const;
  template <MemoQuery Q>
  CellId cell_for(const typename Q::Key& k);
  template <MemoQuery Q>
  const typename Q::Value& value_of(CellId c) const;

  // Records `cells` as one group of `f`, brings each up to date in argument order, and folds
  // their durability into the frame's.
  template <std::size_t N>
  std::array<std::uint32_t, N> fetch_group(Frame& f, std::span<const CellId, N> cells);
  void fetch_dynamic_group(Frame& f, std::span<const CellId> cells);
  // Brings a cell up to verified at the current revision: green by durability, green by its
  // dependencies, or executed (§A1.5).
  void refresh(CellId c);
  void validate(CellId c, std::uint64_t old);
  // Runs a claimed cell's query and stores the result with early cutoff.
  template <MemoQuery Q>
  void execute(CellId c);
  template <MemoQuery Q>
  void store(CellId c, typename Q::Value v, Frame& f);
  [[noreturn]] void cycle(CellId c);
  CellRef ref(CellId c) const { return {cells_[c].key_fp, c}; }
  void observe() noexcept { observed_ = true; }

  std::deque<Cell> cells_;
  std::vector<std::unique_ptr<KindTableBase>> tables_;
  std::vector<std::uint32_t> dep_pool_;
  std::vector<Diagnostic> diag_pool_;
  std::vector<Demand> demand_pool_;
  std::vector<CellId> active_;   // cells being validated or executed, outermost first
  std::array<Revision, durability_levels> last_changed_{};
  Revision revision_ = 1;
  bool observed_ = false;
  Stats stats_;
};

// ---------------------------------------------------------------------------------------------
// The context handles (§A10.2): the only way a query observes the world.

class SyncCtx {
public:
  template <CheapQuery Q>
  typename Q::Value fetch(const typename Q::Key& k) {
    const CellId c = db_->cell_for<Q>(k);
    fetch_one(c);
    return db_->value_of<Q>(c);
  }
  template <InputKind Q>
  const typename Q::Value& fetch(const typename Q::Key& k) {
    const CellId c = db_->cell_for<Q>(k);
    contract_assert(db_->cells_[c].has_value);  // an input is set before it is read
    fetch_one(c);
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
  void fetch_one(CellId c) {
    const std::array<CellId, 1> one{c};
    db_->fetch_group(*frame_, std::span<const CellId, 1>(one));
  }

  Db* db_;
  Frame* frame_;

  friend class Db;
};

// A query's coroutine type (§A3.1). Lazily started; the engine runs it to completion.
template <class V>
class [[nodiscard]] query_task {
public:
  struct promise_type {
    std::optional<V> value;
    std::exception_ptr error;
    query_task get_return_object() noexcept { return query_task(std::coroutine_handle<promise_type>::from_promise(*this)); }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
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

  // Runs the body to completion on this stack. An exception escaping the body is rethrown.
  V run() && {
    complete();
    return take();
  }

  // Awaiting a task (a transparent helper) runs it inline.
  bool await_ready() {
    complete();
    return true;
  }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  V await_resume() { return take(); }

private:
  explicit query_task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
  void complete() {
    h_.resume();
    // Until the scheduler (#15), every engine await completes inline, so a body that suspends
    // awaited something other than the engine's awaitables.
    contract_assert(h_.done());
  }
  V take() {
    if (h_.promise().error) std::rethrow_exception(h_.promise().error);
    return std::move(*h_.promise().value);
  }

  std::coroutine_handle<promise_type> h_;
};

// `co_await cx.fetch<Q>(k)`: the hit path and, for now, the miss path too, in `await_ready`.
template <AsyncQuery Q>
class fetch_awaitable {
public:
  bool await_ready() {
    const std::array<CellId, 1> one{cell_};
    const auto at = db_->fetch_group(*frame_, std::span<const CellId, 1>(one));
    if (loc_) frame_->demands.push_back({at[0], *loc_});
    return true;
  }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  const typename Q::Value& await_resume() const { return db_->value_of<Q>(cell_); }

private:
  friend class Ctx;
  fetch_awaitable(Db& db, Frame& f, CellId c, std::optional<Loc> loc) noexcept : db_(&db), frame_(&f), cell_(c), loc_(loc) {}
  Db* db_;
  Frame* frame_;
  CellId cell_;
  std::optional<Loc> loc_;
};

// One member of a `par_fetch`.
template <class Q>
struct q {
  typename Q::Key key;
};

template <AsyncQuery... Qs>
class par_fetch_awaitable {
public:
  bool await_ready() {
    db_->fetch_group(*frame_, std::span<const CellId, sizeof...(Qs)>(cells_));
    return true;
  }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  std::tuple<const typename Qs::Value&...> await_resume() const {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return std::tuple<const typename Qs::Value&...>(db_->value_of<Qs>(cells_[I])...);
    }(std::index_sequence_for<Qs...>());
  }

private:
  friend class Ctx;
  par_fetch_awaitable(Db& db, Frame& f, std::array<CellId, sizeof...(Qs)> cells) noexcept : db_(&db), frame_(&f), cells_(cells) {}
  Db* db_;
  Frame* frame_;
  std::array<CellId, sizeof...(Qs)> cells_;
};

template <AsyncQuery Q>
class par_map_awaitable {
public:
  bool await_ready() {
    db_->fetch_dynamic_group(*frame_, cells_);
    return true;
  }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  std::vector<std::reference_wrapper<const typename Q::Value>> await_resume() const {
    std::vector<std::reference_wrapper<const typename Q::Value>> out;
    out.reserve(cells_.size());
    for (CellId c : cells_) out.push_back(std::cref(db_->value_of<Q>(c)));
    return out;
  }

private:
  friend class Ctx;
  par_map_awaitable(Db& db, Frame& f, std::vector<CellId> cells) noexcept : db_(&db), frame_(&f), cells_(std::move(cells)) {}
  Db* db_;
  Frame* frame_;
  std::vector<CellId> cells_;
};

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

private:
  friend class Db;
  friend struct DiagsOf;
  Ctx(Db& db, Frame& f) noexcept : SyncCtx(db, f) {}
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

query_task<std::vector<Diagnostic>> DiagsOf::run(Ctx& cx, CellRef target) {
  Db& db = *cx.db_;
  cx.fetch_one(target.id);
  const Cell& t = db.cells_[target.id];
  std::vector<Diagnostic> out(db.diag_pool_.begin() + t.diags_begin, db.diag_pool_.begin() + t.diags_begin + t.diags_len);
  // A snapshot: fetching below may grow the pools.
  const std::vector<std::uint32_t> deps(db.dep_pool_.begin() + t.deps_begin, db.dep_pool_.begin() + t.deps_begin + t.deps_len);
  for (std::uint32_t d : deps) {
    const CellId dep = d & ~group_start;
    cx.fetch_one(dep);
    const Cell& c = db.cells_[dep];
    if (c.has_diags && !c.demanded) {
      const auto& sub = co_await cx.fetch<DiagsOf>(db.ref(dep));
      out.append_range(sub);
    }
  }
  co_return out;
}

template <MemoQuery Q>
KindTable<Q>& Db::table() {
  const std::uint32_t s = kind_slot<Q>();
  if (s >= tables_.size()) tables_.resize(s + 1);
  if (!tables_[s]) {
    auto t = std::make_unique<KindTable<Q>>();
    t->name = kind_name<Q>();
    t->seed = kind_seed<Q>();
    tables_[s] = std::move(t);
  }
  return static_cast<KindTable<Q>&>(*tables_[s]);
}

template <MemoQuery Q>
const KindTable<Q>* Db::find_table() const {
  const std::uint32_t s = kind_slot<Q>();
  return s < tables_.size() ? static_cast<const KindTable<Q>*>(tables_[s].get()) : nullptr;
}

template <MemoQuery Q>
CellId Db::cell_for(const typename Q::Key& k) {
  KindTable<Q>& t = table<Q>();
  if (const auto it = t.index.find(k); it != t.index.end()) return it->second;
  const auto c = static_cast<CellId>(cells_.size());
  contract_assert(c < group_start);
  Cell& cell = cells_.emplace_back();
  cell.kind = kind_slot<Q>();
  cell.slot = static_cast<std::uint32_t>(t.keys.size());
  cell.input = InputKind<Q>;
  if constexpr (InputKind<Q>) cell.durability = durability_of<Q>();
  if constexpr (AsyncQuery<Q>) cell.demanded = DemandedQuery<Q>;
  Hasher h;
  h.fp(t.seed);
  hash_append(h, k);
  cell.key_fp = h.finish();
  t.keys.push_back(k);
  t.values.emplace_back();
  t.index.emplace(k, c);
  return c;
}

template <MemoQuery Q>
const typename Q::Value& Db::value_of(CellId c) const {
  const Cell& cell = cells_[c];
  contract_assert(cell.has_value);
  return *static_cast<const KindTable<Q>&>(*tables_[cell.kind]).values[cell.slot];
}

template <std::size_t N>
std::array<std::uint32_t, N> Db::fetch_group(Frame& f, std::span<const CellId, N> cells) {
  const auto at = f.record(cells);
  for (CellId c : cells) {
    refresh(c);
    f.durability = std::min(f.durability, cells_[c].durability);
  }
  return at;
}

void Db::fetch_dynamic_group(Frame& f, std::span<const CellId> cells) {
  bool first = true;
  if (!f.root) {
    for (CellId c : cells) {
      if (f.position_of(c) != ~0u) continue;
      const std::array<CellId, 1> one{c};
      f.record(std::span<const CellId, 1>(one));
      if (!first) f.deps.back() &= ~group_start;  // one group: only the first member starts it
      first = false;
    }
  }
  for (CellId c : cells) {
    refresh(c);
    f.durability = std::min(f.durability, cells_[c].durability);
  }
}

void Db::refresh(CellId c) {
  Cell& e = cells_[c];
  const std::uint64_t w = e.word.load(std::memory_order_acquire);
  switch (state_of(w)) {
    case State::done:
      if (verified_of(w) == revision_ || e.input) {
        ++stats_.hits;
        return;
      }
      if (verified_of(w) >= last_changed_[std::to_underlying(e.durability)]) {
        e.word.store(make_word(State::done, 0, revision_), std::memory_order_release);
        ++stats_.durable;
        return;
      }
      validate(c, w);
      return;
    case State::empty:
      e.word.store(make_word(State::running, static_cast<std::uint32_t>(active_.size()), verified_of(w)), std::memory_order_release);
      tables_[e.kind]->execute(*this, c);
      return;
    case State::running:
    case State::validating:
      cycle(c);
  }
}

void Db::validate(CellId c, std::uint64_t old) {
  Cell& e = cells_[c];
  e.word.store(make_word(State::validating, static_cast<std::uint32_t>(active_.size()), verified_of(old)), std::memory_order_release);
  active_.push_back(c);
  const Revision since = verified_of(old);
  bool changed = false;
  Durability durability = Durability::high;
  try {
    // Groups in program order; every member of a group, with no short-circuit (§A1.5). The pool
    // may grow while members execute, so it is indexed, never held.
    for (std::uint32_t k = 0; k < e.deps_len && !changed;) {
      do {
        const CellId dep = dep_pool_[e.deps_begin + k] & ~group_start;
        refresh(dep);
        changed = changed || cells_[dep].changed_at > since;
        durability = std::min(durability, cells_[dep].durability);
        ++k;
      } while (k < e.deps_len && !(dep_pool_[e.deps_begin + k] & group_start));
    }
  } catch (...) {
    active_.pop_back();
    e.word.store(old, std::memory_order_release);
    throw;
  }
  active_.pop_back();
  if (!changed) {
    // Durability is not part of the fingerprint: a dependency backdated after re-executing may
    // have lowered its own, and the shortcut must not trust this cell's old one. Every
    // dependency was visited, so the minimum is exact; a cell green by the shortcut needs no
    // update, since lowering a dependency's durability takes a re-execution somewhere below it,
    // caused by an input change that also moved `last_changed_` at this cell's level.
    e.durability = durability;
    e.word.store(make_word(State::done, 0, revision_), std::memory_order_release);
    ++stats_.validated;
    return;
  }
  // Red: re-execute. Dependencies after the changed group may not be fetched by the new run.
  e.word.store(make_word(State::running, static_cast<std::uint32_t>(active_.size()), since), std::memory_order_release);
  tables_[e.kind]->execute(*this, c);
}

template <MemoQuery Q>
void KindTable<Q>::execute(Db& db, CellId c) {
  db.execute<Q>(c);
}

template <MemoQuery Q>
void Db::execute(CellId c) {
  Cell& e = cells_[c];
  const std::uint64_t claimed = e.word.load(std::memory_order_acquire);
  // The word to restore if the execution fails: Empty, or the old Done (§A1.6).
  const std::uint64_t old = e.has_value ? make_word(State::done, 0, verified_of(claimed)) : make_word(State::empty, 0, 0);
  if constexpr (InputKind<Q>) {
    contract_assert(false);  // an input is set before it is read
  } else {
    Frame f;
    f.cell = c;
    active_.push_back(c);
    try {
      const typename Q::Key& key = static_cast<KindTable<Q>&>(*tables_[e.kind]).keys[e.slot];
      if constexpr (CheapQuery<Q>) {
        SyncCtx cx(*this, f);
        auto v = Q::run(cx, key);
        active_.pop_back();
        store<Q>(c, std::move(v), f);
      } else {
        Ctx cx(*this, f);
        auto v = Q::run(cx, key).run();
        active_.pop_back();
        store<Q>(c, std::move(v), f);
      }
    } catch (...) {
      if (!active_.empty() && active_.back() == c) active_.pop_back();
      e.word.store(old, std::memory_order_release);
      throw;
    }
  }
}

template <MemoQuery Q>
void Db::store(CellId c, typename Q::Value v, Frame& f) {
  Cell& e = cells_[c];
  contract_assert(e.state() == State::running);
  // `has_diags` is part of the fingerprint: when a dependency gains or loses diagnostics, this
  // cell's value may be unchanged, but `DiagsOf` above it must stop pruning here or start again.
  bool has_diags = !f.diags.empty();
  for (std::uint32_t d : f.deps) {
    const Cell& dep = cells_[d & ~group_start];
    has_diags = has_diags || (dep.has_diags && !dep.demanded);
  }
  Hasher h;
  hash_append(h, v);
  hash_append(h, f.diags);
  hash_append(h, f.demands);
  hash_append(h, has_diags);
  const Fp128 fp = h.finish();
  ++e.runs;
  ++stats_.executed;
  if (e.has_value && fp == e.fp) {
    ++stats_.backdated;  // early cutoff: the old value stays, `changed_at` does not move
  } else {
    static_cast<KindTable<Q>&>(*tables_[e.kind]).values[e.slot] = std::move(v);
    e.fp = fp;
    e.changed_at = revision_;
    e.has_value = true;
  }
  e.deps_begin = static_cast<std::uint32_t>(dep_pool_.size());
  e.deps_len = static_cast<std::uint32_t>(f.deps.size());
  dep_pool_.append_range(f.deps);
  e.diags_begin = static_cast<std::uint32_t>(diag_pool_.size());
  e.diags_len = static_cast<std::uint32_t>(f.diags.size());
  diag_pool_.append_range(std::move(f.diags));
  e.demands_begin = static_cast<std::uint32_t>(demand_pool_.size());
  e.demands_len = static_cast<std::uint32_t>(f.demands.size());
  demand_pool_.append_range(f.demands);
  e.durability = f.durability;
  e.has_diags = has_diags;
  e.word.store(make_word(State::done, 0, revision_), std::memory_order_release);
  contract_assert(e.changed_at <= revision_);
}

void Db::cycle(CellId c) {
  const auto from = std::ranges::find(active_, c);
  contract_assert(from != active_.end());
  std::vector<CellRef> cells;
  for (auto it = from; it != active_.end(); ++it) cells.push_back(ref(*it));
  const auto name = [&](const CellRef& r) { return tables_[cells_[r.id].kind]->name; };
  std::ranges::sort(cells, [&](const CellRef& a, const CellRef& b) {
    return std::tuple(name(a), a.key) < std::tuple(name(b), b.key);
  });
  std::string what = "internal error: query cycle";
  for (const CellRef& r : cells) what += std::format("\n  {} {:016x}{:016x}", name(r), r.key.hi, r.key.lo);
  throw query_cycle(std::move(what), std::move(cells));
}

template <InputKind Q>
void Db::set(const typename Q::Key& k, typename Q::Value v) {
  // BOOTSTRAP: contracts: GCC 16.2 drops `pre` on a function template instantiated through an
  // import (STATUS, "Bootstrap workarounds"); an assertion in the body is kept.
  contract_assert(live_frames() == 0);
  const CellId c = cell_for<Q>(k);
  Cell& e = cells_[c];
  const Fp128 fp = fingerprint(v);
  if (e.has_value && fp == e.fp) return;
  if (observed_) {
    ++revision_;
    observed_ = false;
  }
  static_cast<KindTable<Q>&>(*tables_[e.kind]).values[e.slot] = std::move(v);
  e.fp = fp;
  e.has_value = true;
  e.changed_at = revision_;
  // Every durability at or below the input's has now changed.
  for (std::size_t d = 0; d <= std::to_underlying(e.durability); ++d) last_changed_[d] = revision_;
  e.word.store(make_word(State::done, 0, revision_), std::memory_order_release);
}

template <MemoQuery Q>
decltype(auto) Db::get(const typename Q::Key& k) {
  observe();
  Frame root;
  root.root = true;
  const CellId c = cell_for<Q>(k);
  const std::array<CellId, 1> one{c};
  fetch_group(root, std::span<const CellId, 1>(one));
  if constexpr (CheapQuery<Q>) return typename Q::Value(value_of<Q>(c));
  else return value_of<Q>(c);
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
  const KindTable<Q>* t = find_table<Q>();
  if (!t) return 0;
  const auto it = t->index.find(k);
  return it == t->index.end() ? 0 : cells_[it->second].runs;
}

template <MemoQuery Q>
Revision Db::changed_at(const typename Q::Key& k) const {
  const KindTable<Q>* t = find_table<Q>();
  if (!t) return 0;
  const auto it = t->index.find(k);
  return it == t->index.end() ? 0 : cells_[it->second].changed_at;
}

template <MemoQuery Q>
std::vector<std::vector<CellRef>> Db::dependencies(const typename Q::Key& k) const {
  std::vector<std::vector<CellRef>> out;
  const KindTable<Q>* t = find_table<Q>();
  if (!t) return out;
  const auto it = t->index.find(k);
  if (it == t->index.end()) return out;
  const Cell& e = cells_[it->second];
  for (std::uint32_t d : std::span(dep_pool_).subspan(e.deps_begin, e.deps_len)) {
    if (d & group_start) out.emplace_back();
    out.back().push_back(ref(d & ~group_start));
  }
  return out;
}

template <MemoQuery Q>
std::vector<Demand> Db::demands(const typename Q::Key& k) const {
  const KindTable<Q>* t = find_table<Q>();
  if (!t) return {};
  const auto it = t->index.find(k);
  if (it == t->index.end()) return {};
  const Cell& e = cells_[it->second];
  const auto list = std::span(demand_pool_).subspan(e.demands_begin, e.demands_len);
  return {list.begin(), list.end()};
}

}  // namespace ycxx::engine
