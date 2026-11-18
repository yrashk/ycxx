// A Db without a pool runs one request at a time: a second thread's request while one runs
// violates `Request::begin`'s contract. The handler reports and exits; CTest checks its message.
import std;
import ycxx.engine;

using namespace ycxx::engine;

void handle_contract_violation(const std::contracts::contract_violation& v) {
  std::println("violated: {}", v.comment());
  std::exit(0);
}

namespace {

Db* the_db = nullptr;

struct [[=input{Durability::low}]] In {
  using Key = std::uint32_t;
  using Value = std::int64_t;
};
struct Echo {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return cx.fetch<In>(k); }
};
// Runs, on the first thread, a request on another thread, and waits for it.
struct MakesAnotherRequest {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    std::jthread([] { (void)the_db->get<Echo>(1); }).join();
    co_return cx.fetch<In>(k);
  }
};

}  // namespace

int main() {
  Db db;
  the_db = &db;
  db.set<In>(0, 1);
  db.set<In>(1, 2);
  (void)db.get<MakesAnotherRequest>(0);
  return 1;
}
