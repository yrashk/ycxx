// Register an input and a memoized query, then change the input and demand the query again.
import std;
import ycxx.engine;

namespace engine = ycxx::engine;

struct [[=engine::input{engine::Durability::low}]] Text {
  using Key = int;
  using Value = int;
};

struct Twice {
  using Key = int;
  using Value = int;
  static engine::query_task<Value> run(engine::Ctx& ctx, Key key) {
    co_return 2 * ctx.fetch<Text>(key);
  }
};

int main() {
  engine::Db db;
  db.set<Text>(0, 20);
  if (db.get<Twice>(0) != 40)
    return 1;
  if (db.get<Twice>(0) != 40 || db.runs<Twice>(0) != 1)
    return 2;
  db.set<Text>(0, 21);
  return db.get<Twice>(0) == 42 && db.runs<Twice>(0) == 2 ? 0 : 3;
}
