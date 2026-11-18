// Measures the costs the query engine's budget rests on (§A3.3): a fetch hit from inside a
// query (hash the key, probe the table, check the state word, record the dependency), a root
// `get` hit, and hashing a 16- and 32-byte key. Prints nanoseconds per operation.
import std;
import ycxx.engine;

using namespace ycxx::engine;

namespace {

struct [[=input{Durability::low}]] In {
  using Key = std::uint32_t;
  using Value = std::int64_t;
};
struct Leaf {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return cx.fetch<In>(k % 64); }
};
constexpr std::uint32_t fanout = 4096;
// One execution that fetches `fanout` cells already verified at this revision: all hits.
struct Fan {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    std::int64_t sum = 0;
    for (std::uint32_t i = 0; i < fanout; ++i) sum += co_await cx.fetch<Leaf>(i);
    co_return sum + k;
  }
};

struct Key16 {
  std::uint32_t a, b, c, d;
};
struct Key32 {
  std::uint64_t a, b, c, d;
};

double ns_per(auto&& f, std::size_t n) {
  const auto t0 = std::chrono::steady_clock::now();
  f();
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n);
}

}  // namespace

int main() {
  Db db;
  for (std::uint32_t i = 0; i < 64; ++i) db.set<In>(i, i);
  for (std::uint32_t i = 0; i < fanout; ++i) (void)db.get<Leaf>(i);
  constexpr std::uint32_t rounds = 200;
  // Each Fan(k) is a new cell, so it executes; its fetches are all hits.
  const double fetch = ns_per([&] { for (std::uint32_t r = 0; r < rounds; ++r) (void)db.get<Fan>(r); }, std::size_t{rounds} * fanout);
  std::int64_t sink = 0;
  const double get = ns_per([&] { for (std::uint32_t r = 0; r < 1'000'000; ++r) sink += db.get<Leaf>(r % fanout); }, 1'000'000);
  ycxx::Fp128 acc;
  const double h16 = ns_per([&] { for (std::uint32_t r = 0; r < 1'000'000; ++r) acc.lo ^= ycxx::fingerprint(Key16{r, 2, 3, 4}).lo; }, 1'000'000);
  const double h32 = ns_per([&] { for (std::uint64_t r = 0; r < 1'000'000; ++r) acc.lo ^= ycxx::fingerprint(Key32{r, 2, 3, 4}).lo; }, 1'000'000);
  std::println("fetch hit inside a query: {:.1f} ns", fetch);
  std::println("root get hit:             {:.1f} ns", get);
  std::println("fingerprint 16-byte key:  {:.1f} ns", h16);
  std::println("fingerprint 32-byte key:  {:.1f} ns", h32);
  std::println("(checksum {} {})", sink, acc.lo);
}
