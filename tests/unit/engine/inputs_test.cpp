// The engine's additions for the daemon (#95, daemon design §4.4, §5.1, §6.3, §8.1): input
// providers, the `editor` and `ambient` durabilities, `set` reporting a change,
// `reset_unobserved`, and counts per request and per query kind.
import std;
import ycxx.execution;
import ycxx.engine;
import ycxx.unit;

using namespace ycxx::engine;
using ycxx::unit::check;
namespace ex = ycxx::exec;

namespace {

const unsigned hw = std::max(2u, std::thread::hardware_concurrency());

// A file's text, provided on first read; `high` for paths under "/sys", `low` otherwise.
struct [[=input{Durability::low}]] File {
  using Key = std::uint32_t;  // a path id; ids >= 1000 are system headers
  using Value = std::string;
};
struct [[=input{Durability::editor}]] Buffer {
  using Key = std::uint32_t;
  using Value = std::string;
};
struct [[=input{Durability::ambient}]] BuildTime {
  using Key = std::uint32_t;
  using Value = std::string;
};

std::atomic<int> provided_calls{0};
provided<std::string> provide_file(const std::uint32_t& path) {
  provided_calls.fetch_add(1);
  return {std::format("file{}", path), path >= 1000 ? Durability::high : Durability::low};
}

struct Size {
  using Key = std::uint32_t;
  using Value = std::size_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return cx.fetch<File>(k).size(); }
};
// The disk view and the editor view of one file (daemon design §6.1).
struct DiskView {
  using Key = std::uint32_t;
  using Value = std::size_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return 1 + co_await cx.fetch<Size>(k); }
};
struct EditorView {
  using Key = std::uint32_t;
  using Value = std::size_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    const std::string& b = cx.fetch<Buffer>(k);
    co_return b.empty() ? co_await cx.fetch<DiskView>(k) : b.size();
  }
};
struct Stamp {
  using Key = std::uint32_t;
  using Value = std::string;
  static query_task<Value> run(Ctx& cx, Key k) { co_return cx.fetch<BuildTime>(0) + cx.fetch<File>(k); }
};
// Many files at once, so concurrent first reads race to provide.
struct Many {
  using Key = std::uint32_t;
  using Value = std::size_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    std::vector<std::uint32_t> files;
    for (std::uint32_t i = 0; i < 16; ++i) files.push_back((k + i) % 32);
    const auto& sizes = co_await cx.par_map<Size>(files);
    std::size_t sum = 0;
    for (const auto& s : sizes) sum += s.get();
    co_return sum;
  }
};

void providers() {
  Db db;
  db.set_provider<File>(provide_file);
  provided_calls = 0;
  const Revision r = db.revision();
  check(db.get<Size>(7) == 5, "an input nobody set is provided on first read");
  check(db.revision() == r && db.changed_at<File>(7) == r, "at this revision, with no new one");
  check(db.get<Size>(1001) == 8 && provided_calls == 2, "the provider runs once per cell");
  check(db.get<File>(7) == "file7" && provided_calls == 2, "a provided input is read like a set one");

  // The provider's durability: an edit of a project file leaves the system header's cells green
  // by the shortcut (§A1.4).
  check(db.set<File>(7, "edited"), "set reports a change");
  check(!db.set<File>(7, "edited"), "and no change for the same value");
  db.reset_stats();
  check(db.get<DiskView>(1001) == 9 && db.stats().executed == 1, "cold: the system header's new cell");
  check(db.set<File>(7, "edited again"), "another project edit");
  db.reset_stats();
  check(db.get<DiskView>(1001) == 9 && db.stats().executed == 0 && db.stats().durable == 1,
        "a high-durability provided input keeps its dependents green in one comparison");
  check(db.get<DiskView>(7) == 13, "and the project file's dependents see the edit");

  // Concurrent first reads of the same files: each provided once.
  Pool pool(hw, 95);
  Db many(pool);
  many.set_provider<File>(provide_file);
  provided_calls = 0;
  std::vector<std::size_t> got(32);
  {
    ex::counting_scope scope;
    for (std::uint32_t k = 0; k < 32; ++k) {
      ex::spawn(many.request<Many>(k) | ex::then([&got, k](const std::size_t& v) noexcept { got[k] = v; }) |
                    ex::upon_error([](std::exception_ptr) noexcept {}),
                scope.get_token());
    }
    ycxx::exec::sync_wait(scope.join());
  }
  check(provided_calls == 32, std::format("32 files, provided once each under contention ({})", provided_calls.load()));
  bool right = true;
  for (std::uint32_t k = 0; k < 32; ++k) {
    std::size_t want = 0;
    for (std::uint32_t i = 0; i < 16; ++i) want += std::format("file{}", (k + i) % 32).size();
    right = right && got[k] == want;
  }
  check(right, "every request read the provided values");

  // A provider that throws: the request fails, and the cell is provided again later.
  Db failing;
  bool fail = true;
  failing.set_provider<File>([&](const std::uint32_t& p) -> provided<std::string> {
    if (fail) throw std::runtime_error("unreadable");
    return {std::format("ok{}", p), Durability::low};
  });
  bool thrown = false;
  try {
    (void)failing.get<Size>(3);
  } catch (const std::runtime_error&) {
    thrown = true;
  }
  fail = false;
  failing.set<File>(99, "x");  // a new revision, so the failed request's cell runs again
  check(thrown && failing.get<Size>(3) == 3, "a failed provider stores nothing; the next read provides again");
}

// A keystroke (`editor`) leaves every disk-view cell green by the shortcut (daemon design §6.3).
void editor_durability() {
  Db db;
  db.set_provider<File>(provide_file);
  db.set<Buffer>(4, "");
  db.set<BuildTime>(0, "t0");
  (void)db.get<EditorView>(4);
  (void)db.get<DiskView>(5);
  db.set<Buffer>(4, "typed");
  db.reset_stats();
  check(db.get<DiskView>(5) == 6 && db.stats().executed == 0 && db.stats().validated == 0 && db.stats().durable == 1,
        "a disk-view cell after a keystroke: one comparison");
  check(db.get<EditorView>(4) == 5, "the editor view sees the buffer");
  db.set<BuildTime>(0, "t1");
  db.reset_stats();
  check(db.get<DiskView>(5) == 6 && db.stats().durable == 1 && db.stats().executed == 0,
        "an `ambient` change leaves `low` cells green too");
  check(db.get<Stamp>(5) == "t1file5", "and reaches the cells that read it");
}

// `reset_unobserved` (daemon design §4.4).
void reset_unobserved() {
  Db db;
  db.set_provider<File>(provide_file);
  db.set<BuildTime>(0, "t0");
  const Revision r = db.revision();
  check(db.reset_unobserved<BuildTime>(0, "t1"), "nothing read it: reset in place");
  check(db.revision() == r && db.get<Stamp>(1) == "t1file1", "no new revision; the next read sees it");
  check(!db.reset_unobserved<BuildTime>(0, "t2"), "read since: refused");
  check(db.get<Stamp>(1) == "t1file1", "and nothing changed");
  db.set<BuildTime>(0, "t3");
  check(db.reset_unobserved<BuildTime>(0, "t4"), "a set clears the mark");
  check(db.get<Stamp>(1) == "t4file1", "the reader computed before the set re-runs with the reset value");
  check(db.reset_unobserved<BuildTime>(7, "new"), "an input nobody set or read takes its first value");

  // Readers race with a reset on a pool, round after round: a reset succeeds only if no reader
  // read before it, so every reader of a round reads the same whole value, the reset one when it
  // succeeded.
  Pool pool(hw, 44);
  Db live(pool);
  live.set_provider<File>(provide_file);
  const auto value = [](int i, char salt) { return std::string(256, static_cast<char>((salt == 'r' ? 'A' : 'a') + i % 26)); };
  std::mt19937 rng(44);
  int succeeded = 0, failed = 0, mixed = 0;
  for (int round = 0; round < 300; ++round) {
    live.set<BuildTime>(0, value(round, 's'));
    std::vector<std::string> read(8);
    // A third of the rounds reset before the readers start, the rest race with them.
    const bool early = round % 3 == 0;
    const bool early_reset = early && live.reset_unobserved<BuildTime>(0, value(round, 'r'));
    ex::counting_scope scope;
    for (std::uint32_t j = 0; j < read.size(); ++j) {
      ex::spawn(live.request<Stamp>(static_cast<std::uint32_t>(round * 8 + j)) |
                    ex::then([&read, j](const std::string& v) noexcept { read[j] = v.substr(0, 256); }) |
                    ex::upon_error([](std::exception_ptr) noexcept {}),
                scope.get_token());
    }
    for (auto spin = rng() % 3000; spin > 0; --spin) {
      if (spin % 500 == 0) std::this_thread::yield();
    }
    const bool reset = early ? early_reset : live.reset_unobserved<BuildTime>(0, value(round, 'r'));
    ycxx::exec::sync_wait(scope.join());
    (reset ? succeeded : failed)++;
    const std::string want = reset ? value(round, 'r') : value(round, 's');
    for (const auto& r : read) mixed += r != want;
  }
  check(mixed == 0, std::format("every reader of a round read the same value ({} did not)", mixed));
  check(succeeded > 0 && failed > 0, std::format("resets both won and lost the race ({} won, {} lost)", succeeded, failed));
}

// A sender made before a `set` and started after it reads the new revision, and the next `set`
// still starts a revision (review of #95, P0).
void sender_across_a_set() {
  Db db;
  db.set<BuildTime>(0, "a");
  auto s = db.request<Stamp>(3);
  db.set_provider<File>(provide_file);
  db.set<BuildTime>(0, "bb");
  check(std::get<0>(*ycxx::exec::sync_wait(std::move(s))) == "bbfile3", "the sender reads the revision it started at");
  db.set<BuildTime>(0, "cccc");
  check(db.get<Stamp>(3) == "ccccfile3", "and the next set is a new revision");
}

// Races between readers and writers of inputs on a pool.
void input_races() {
  Pool pool(hw, 120);
  // `get<Input>` against resets: every read whole, one value per round.
  Db db(pool);
  const auto value = [](int i, char salt) { return std::string(256, static_cast<char>((salt == 'r' ? 'A' : 'a') + i % 26)); };
  int mixed = 0;
  for (int round = 0; round < 200; ++round) {
    db.set<BuildTime>(0, value(round, 's'));
    std::vector<std::string> read(4);
    {
      std::vector<std::jthread> readers;
      for (std::size_t j = 0; j < read.size(); ++j) readers.emplace_back([&, j] { read[j] = db.get<BuildTime>(0); });
      (void)db.reset_unobserved<BuildTime>(0, value(round, 'r'));
    }
    const std::string after = db.get<BuildTime>(0);
    for (const auto& r : read) mixed += r != after;
  }
  check(mixed == 0, std::format("get against reset: every read the final value of its round ({} not)", mixed));

  // A reset racing the provider of an empty cell: one of them defines it, and every reader agrees.
  Db fresh(pool);
  fresh.set_provider<File>(provide_file);
  int disagree = 0;
  for (std::uint32_t k = 0; k < 200; ++k) {
    std::vector<std::size_t> sizes(6);
    ex::counting_scope scope;
    for (std::size_t j = 0; j < sizes.size(); ++j) {
      ex::spawn(fresh.request<Size>(k) | ex::then([&sizes, j](const std::size_t& v) noexcept { sizes[j] = v; }) |
                    ex::upon_error([](std::exception_ptr) noexcept {}),
                scope.get_token());
    }
    (void)fresh.reset_unobserved<File>(k, std::string(100 + k, 'x'));
    ycxx::exec::sync_wait(scope.join());
    const std::size_t want = fresh.get<File>(k).size();
    for (std::size_t v : sizes) disagree += v != want;
  }
  check(disagree == 0, std::format("provider against reset: readers agree with the cell ({} not)", disagree));

  // A provider that always throws: every reader gets the error, none a value.
  Db failing(pool);
  failing.set_provider<File>([](const std::uint32_t&) -> provided<std::string> { throw std::runtime_error("unreadable"); });
  std::atomic<int> values{0}, errors{0};
  {
    ex::counting_scope scope;
    for (int j = 0; j < 8; ++j) {
      ex::spawn(failing.request<Size>(9) | ex::then([&](const std::size_t&) noexcept { ++values; }) |
                    ex::upon_error([&](std::exception_ptr) noexcept { ++errors; }),
                scope.get_token());
    }
    ycxx::exec::sync_wait(scope.join());
  }
  check(values == 0 && errors == 8, "a throwing provider fails every reader");
}

std::atomic<bool> held{true};
std::atomic<bool> holding{false};
struct Held {
  using Key = std::uint32_t;
  using Value = std::size_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    const std::size_t a = co_await cx.fetch<Size>(k);
    holding = true;
    while (held.load()) {
      co_await cx.stop_point();
      std::this_thread::yield();
    }
    co_return a;
  }
};

// Counts per request and per kind (daemon design §8.1).
void counts() {
  Pool pool(hw);
  Db db(pool);
  db.set_provider<File>(provide_file);
  Counters a, b, waiter;
  {
    ex::counting_scope scope;
    ex::spawn(db.request<DiskView>(100, Priority::background, &a) | ex::then([](const std::size_t&) noexcept {}) |
                  ex::upon_error([](std::exception_ptr) noexcept {}),
              scope.get_token());
    ex::spawn(db.request<Many>(0, Priority::background, &b) | ex::then([](const std::size_t&) noexcept {}) |
                  ex::upon_error([](std::exception_ptr) noexcept {}),
              scope.get_token());
    ycxx::exec::sync_wait(scope.join());
  }
  check(a.stats().executed == 2, "a request counts its own executions");
  check(b.stats().executed + a.stats().executed == db.stats().executed, "and the two requests' counts add up to the Db's");
  (void)ycxx::exec::sync_wait(db.request<DiskView>(100, Priority::background, &waiter));
  check(waiter.stats().executed == 0 && waiter.stats().hits == 1, "a request that finds its cell done executes nothing");

  db.count_per_kind(true);
  db.reset_stats();
  (void)ycxx::exec::sync_wait(db.request<Many>(40));
  const auto per = db.stats_per_kind();
  const auto find = [&](std::string_view name) {
    for (const auto& [k, s] : per) {
      if (k.contains(name)) return s;
    }
    return Stats{};
  };
  check(find("Many").executed == 1 && find("Size").executed == 8, "executions per kind (8 sizes were new)");
  check(std::ranges::is_sorted(per, {}, &std::pair<std::string_view, Stats>::first), "kinds by name");

  // A ticket's counts and restarts across epochs.
  Epochs ep(db);
  std::atomic<int> answered{0};
  auto t = ep.spawn<Many>(77, Priority::background, [&](const std::size_t&) { ++answered; }, [](std::exception_ptr) {});
  ep.join();
  check(answered == 1 && t->restarts() == 0 && t->stats().executed > 0, "a ticket counts its runs");

  // Restarts: the ticket's counts accumulate over its runs.
  held = true;
  holding = false;
  auto h = ep.spawn<Held>(500, Priority::background, [&](const std::size_t&) { ++answered; }, [](std::exception_ptr) {});
  while (!holding.load()) std::this_thread::yield();
  const auto first = h->stats().executed;  // Size(500): its own execution
  holding = false;
  (void)ep.apply([](Db&) {});
  while (!holding.load()) std::this_thread::yield();
  held = false;
  ep.join();
  check(h->restarts() == 1 && first == 1 && h->stats().executed == 2 && answered == 2,
        "one restart; counts accumulate (Size once, Held once)");
}

}  // namespace

int main() {
  providers();
  editor_durability();
  reset_unobserved();
  sender_across_a_set();
  input_races();
  counts();
  return ycxx::unit::result("engine_inputs");
}
