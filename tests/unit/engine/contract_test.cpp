// Revisions advance only at quiescence (§A4.1): setting an input while a query runs violates
// `Db::set`'s precondition. The handler reports and exits; CTest checks its message.
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
struct SetsAnInput {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    the_db->set<In>(k, 2);  // impure on purpose
    co_return cx.fetch<In>(k);
  }
};

}  // namespace

int main() {
  Db db;
  the_db = &db;
  db.set<In>(0, 1);
  (void)db.get<SetsAnInput>(0);
  return 1;
}
