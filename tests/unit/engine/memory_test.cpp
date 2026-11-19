// Memory in the engine (#274): a cell that allocates a known amount shows it in its event and its
// kind; a cell's own numbers leave out what it demanded, async or cheap; a frame suspended on one
// worker and resumed on another keeps counting into itself; inputs are measured by their provider
// or by walking a value set from outside; re-executing a cell replaces its retained bytes; the
// process's totals and a window's peak see a run's allocations.
import std;
import ycxx.engine;
import ycxx.memory;
import ycxx.unit;

using namespace ycxx::engine;
using ycxx::unit::check;
namespace memory = ycxx::memory;
namespace ex = std::execution;

namespace {

constexpr std::int64_t KiB = 1024;
constexpr std::int64_t MiB = 1024 * KiB;
// What the engine allocates on a frame's behalf besides its body's own (its dependency list, new
// cells' entries, the `Ctx`'s helpers).
constexpr std::int64_t slack = 64 * KiB;

bool near(std::int64_t got, std::int64_t want) { return got >= want && got < want + slack; }

class Events final : public Tracer {
 public:
  void event(TraceEvent e) noexcept override {
    std::lock_guard g(m_);
    all_.push_back(std::move(e));
  }
  // The last event of a cell (its kind's name ends with `kind_part`, `::Keep`; its key as text),
  // with an outcome.
  std::optional<TraceEvent> of(std::string_view kind_part, std::string_view key, std::optional<Outcome> o = std::nullopt) {
    std::lock_guard g(m_);
    for (auto it = all_.rbegin(); it != all_.rend(); ++it) {
      if (it->kind.ends_with(kind_part) && it->key == key && (!o || it->outcome == *o)) return *it;
    }
    return std::nullopt;
  }
  void clear() {
    std::lock_guard g(m_);
    all_.clear();
  }

 private:
  std::mutex m_;
  std::vector<TraceEvent> all_;
};

struct [[=input{Durability::low}]] Size {
  using Key = std::uint32_t;
  using Value = std::uint64_t;  // bytes
};
struct [[=input{Durability::low}]] Text {
  using Key = std::uint32_t;
  using Value = std::string;
};

using Bytes = std::vector<std::uint64_t>;
Bytes bytes(std::int64_t n, std::uint64_t seed) { return Bytes(static_cast<std::size_t>(n) / sizeof(std::uint64_t), seed); }

// Keeps `Size(k)` bytes in its value.
struct Keep {
  using Key = std::uint32_t;
  using Value = Bytes;
  static query_task<Value> run(Ctx& cx, Key k) { co_return bytes(static_cast<std::int64_t>(cx.fetch<Size>(k)), k); }
};
// Allocates `Size(k)` bytes for a while, and keeps nothing of them.
struct Scratch {
  using Key = std::uint32_t;
  using Value = std::uint64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    const Bytes b = bytes(static_cast<std::int64_t>(cx.fetch<Size>(k)), k);
    co_return b.size();
  }
};
// Keeps 256 KiB whose contents depend only on `Size(k)`'s parity: a change of size that keeps
// the parity re-executes it to the same value (early cutoff).
struct Parity {
  using Key = std::uint32_t;
  using Value = Bytes;
  static query_task<Value> run(Ctx& cx, Key k) { co_return bytes(256 * KiB, cx.fetch<Size>(k) % 2); }
};
// A cheap kind that keeps 256 KiB.
struct [[=cheap]] CheapKeep {
  using Key = std::uint32_t;
  using Value = Bytes;
  static Value run(SyncCtx& cx, Key k) { return bytes(256 * KiB + static_cast<std::int64_t>(cx.fetch<Size>(k)) * 0, k); }
};
// Keeps 128 KiB of its own, and demands `Keep(k)` and `CheapKeep(k)`, whose value it copies.
struct Parent {
  using Key = std::uint32_t;
  using Value = Bytes;
  static query_task<Value> run(Ctx& cx, Key k) {
    const Bytes& child = co_await cx.fetch<Keep>(k);
    const Bytes cheap = cx.fetch<CheapKeep>(k);
    Bytes mine = bytes(128 * KiB, child.size() + cheap.size());
    co_return mine;
  }
};

// A child that takes a while, so its parent suspends on it.
struct Slow {
  using Key = std::uint32_t;
  using Value = std::uint64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    co_return cx.fetch<Size>(k);
  }
};
Pool* the_pool = nullptr;
std::atomic<int> moved{0};  // `Split` runs that resumed on another worker
// Allocates 64 KiB, waits for `Slow(k)`, then allocates 192 KiB more, all kept: 256 KiB in all,
// half of it typically on another worker than the first.
struct Split {
  using Key = std::uint32_t;
  using Value = std::pair<Bytes, Bytes>;
  static query_task<Value> run(Ctx& cx, Key k) {
    const auto before = the_pool->worker_index();
    Bytes first = bytes(64 * KiB, k);
    const std::uint64_t s = co_await cx.fetch<Slow>(k);
    if (the_pool->worker_index() != before) moved.fetch_add(1);
    Bytes second = bytes(192 * KiB, s);
    co_return Value{std::move(first), std::move(second)};
  }
};
// Many `Split`s at once, so frames wait and move between workers.
struct Splits {
  using Key = std::uint32_t;
  using Value = std::uint64_t;
  static query_task<Value> run(Ctx& cx, Key n) {
    std::vector<std::uint32_t> keys;
    for (std::uint32_t i = 0; i < n; ++i) keys.push_back(100 + i);
    const auto& got = co_await cx.par_map<Split>(keys);
    co_return got.size();
  }
};

// A provider of 300,000 bytes (more than `slack`, so its reader would show them), and a reader.
provided<std::string> provide_text(const std::uint32_t& k) { return {std::string(300000, static_cast<char>('a' + k % 26)), Durability::low}; }
struct Reads {
  using Key = std::uint32_t;
  using Value = std::uint64_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return cx.fetch<Text>(k).size(); }
};

void known_amounts() {
  Db db;
  Events ev;
  db.set<Size>(1, 1 * MiB);
  db.set<Size>(2, 2 * MiB);
  (void)db.get<Keep>(1, &ev);
  const auto keep = ev.of("::Keep", "1", Outcome::executed);
  check(keep.has_value(), "the cell's event");
  check(keep && near(static_cast<std::int64_t>(keep->alloc_bytes), MiB), std::format("it allocated 1 MiB ({})", keep ? keep->alloc_bytes : 0));
  check(keep && near(keep->retained_bytes, MiB), std::format("and retains it ({})", keep ? keep->retained_bytes : 0));
  check(keep && near(keep->peak_bytes, MiB), "its peak is what it kept");
  check(keep && keep->allocs >= 1 && keep->allocs < 64, std::format("in a few allocations ({})", keep ? keep->allocs : 0));

  (void)db.get<Scratch>(2, &ev);
  const auto scratch = ev.of("::Scratch", "2", Outcome::executed);
  check(scratch && near(scratch->peak_bytes, 2 * MiB), std::format("a temporary's peak ({})", scratch ? scratch->peak_bytes : 0));
  check(scratch && scratch->retained_bytes < slack, std::format("and nothing retained ({})", scratch ? scratch->retained_bytes : 0));

  // A reuse allocates nothing, and shows what the cell retains.
  ev.clear();
  (void)db.get<Keep>(1, &ev);
  const auto again = ev.of("::Keep", "1");
  check(again && again->outcome == Outcome::reused && again->alloc_bytes == 0 && near(again->retained_bytes, MiB),
        "a reuse: no allocation, the cell's retained bytes");

  // The kind's sum; a re-execution replaces the cell's bytes instead of adding to them.
  const auto kind_retained = [&](std::string_view part) {
    for (const KindMemory& k : db.memory_per_kind()) {
      if (k.kind.ends_with(part)) return std::pair(k.retained, k.values);
    }
    return std::pair<std::int64_t, std::uint64_t>(0, 0);
  };
  check(near(kind_retained("::Keep").first, MiB) && kind_retained("::Keep").second == 1, "the kind retains the cell's bytes");
  db.set<Size>(1, 3 * MiB);
  (void)db.get<Keep>(1);
  check(near(kind_retained("::Keep").first, 3 * MiB) && kind_retained("::Keep").second == 1,
        std::format("re-executed: replaced, not added ({})", kind_retained("::Keep").first));

  // Early cutoff: the new value is dropped and the old one kept, and so is the measure.
  db.set<Size>(9, 2);
  (void)db.get<Parity>(9);
  db.set<Size>(9, 4);
  ev.clear();
  (void)db.get<Parity>(9, &ev);
  const auto cut = ev.of("::Parity", "9", Outcome::cutoff);
  check(cut && near(cut->retained_bytes, 256 * KiB) && near(kind_retained("::Parity").first, 256 * KiB),
        std::format("a cutoff retains the one value it keeps ({})", cut ? cut->retained_bytes : 0));

  // Inputs: set from outside, sized by walking the value; given by a provider, measured.
  db.set<Text>(1, std::string(10000, 'x'));
  check(kind_retained("::Text").first == 10001, std::format("a set string's capacity ({})", kind_retained("::Text").first));
  db.set<Text>(1, std::string(20000, 'y'));
  check(kind_retained("::Text").first == 20001, "set again: replaced");
  Db provided;
  provided.set_provider<Text>(provide_text);
  Events pev;
  (void)provided.get<Reads>(7, &pev);
  const auto input = pev.of("::Text", "7", Outcome::input);
  check(input && near(static_cast<std::int64_t>(input->alloc_bytes), 300000) && near(input->retained_bytes, 300000),
        std::format("a provided input: what its provider allocated ({}, {})", input ? input->alloc_bytes : 0, input ? input->retained_bytes : 0));
  const auto reader = pev.of("::Reads", "7", Outcome::executed);
  check(reader && reader->alloc_bytes < static_cast<std::uint64_t>(slack), "is not its reader's");
}

void nested() {
  Db db;
  Events ev;
  db.set<Size>(5, 512 * KiB);
  (void)db.get<Parent>(5, &ev);
  const auto parent = ev.of("::Parent", "5", Outcome::executed);
  const auto child = ev.of("::Keep", "5", Outcome::executed);
  const auto cheap = ev.of("::CheapKeep", "5", Outcome::executed);
  check(child && near(child->retained_bytes, 512 * KiB), std::format("the async child's own ({}, {})", child ? child->alloc_bytes : 0, child ? child->retained_bytes : 0));
  check(cheap && near(cheap->retained_bytes, 256 * KiB), std::format("the cheap child's own ({})", cheap ? cheap->retained_bytes : 0));
  // Its own 128 KiB, and its copy of the cheap child's value (a cheap fetch returns a copy).
  check(parent && near(static_cast<std::int64_t>(parent->alloc_bytes), 384 * KiB) && near(parent->retained_bytes, 128 * KiB),
        std::format("the parent's own, without its children's ({}, {})", parent ? parent->alloc_bytes : 0, parent ? parent->retained_bytes : 0));
  check(cheap && parent && cheap->parent == parent->id && child && child->parent == parent->id, "both are under the parent");
}

void across_workers() {
  const unsigned threads = std::max(4u, std::thread::hardware_concurrency());
  bool every = true;
  for (std::uint64_t seed : {0ull, 7ull, 274ull}) {
    Pool pool(threads, seed == 0 ? std::nullopt : std::optional(seed));
    the_pool = &pool;
    Db db(pool);
    Events ev;
    for (std::uint32_t i = 0; i < 32; ++i) db.set<Size>(100 + i, i);
    moved = 0;
    (void)db.get<Splits>(32, &ev);
    for (std::uint32_t i = 0; i < 32; ++i) {
      const auto e = ev.of("::Split", std::to_string(100 + i), Outcome::executed);
      const bool right = e && near(static_cast<std::int64_t>(e->alloc_bytes), 256 * KiB) && near(e->retained_bytes, 256 * KiB);
      if (!right) {
        std::println(std::cerr, "Split({}) with seed {}: alloc {}, retained {}, workers {} -> {}", 100 + i, seed, e ? e->alloc_bytes : 0,
                     e ? e->retained_bytes : 0, e ? e->worker : -2, e ? e->end_worker : -2);
      }
      every = every && right;
    }
    check(moved > 0, std::format("frames resumed on other workers (seed {}: {} of 32)", seed, moved.load()));
    the_pool = nullptr;
  }
  check(every, "a frame counts what it allocates before and after a wait, on whichever worker");
}

void process_totals() {
  const memory::Totals before = memory::process();
  memory::Window w;
  {
    const Bytes temp = bytes(4 * MiB, 1);
    const memory::Totals during = memory::process();
    check(during.current - before.current >= 4 * MiB, "the process's current bytes see a live allocation");
    check(during.allocated - before.allocated >= static_cast<std::uint64_t>(4 * MiB) && during.count > before.count,
          "cumulative bytes and count");
  }
  const memory::Totals after = memory::process();
  check(after.current - before.current < MiB, "freed: current falls back");
  check(w.peak() - before.current >= 4 * MiB, std::format("the window's peak saw it ({})", w.peak() - before.current));
  check(after.peak >= w.peak(), "and the process's");
}

}  // namespace

int main() {
  check(memory::installed() && memory::counting(), "the allocation functions count");
  known_amounts();
  nested();
  across_workers();
  process_totals();
  return ycxx::unit::result("engine_memory");
}
