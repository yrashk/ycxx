#include <stdlib.h>

import std;
import ycxx.engine;
import ycxx.unit;

namespace ycxx::engine::snapshot_test {

using ycxx::unit::check;

struct [[=input{Durability::low}]] Text {
  using Key = std::uint32_t;
  using Value = std::string;
};

struct Length {
  using Key = Text::Key;
  using Value = std::uint64_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return cx.fetch<Text>(k).size(); }
};

struct [[=cheap]] Twice {
  using Key = Text::Key;
  using Value = std::uint64_t;
  static Value run(SyncCtx& cx, Key k) { return cx.fetch<Text>(k).size() * 2; }
};

template <class Q>
void round_trip(const std::string& path, bool changed) {
  constexpr Fp128 build{1, 2};
  constexpr q<Text> input{0};
  Db producer;
  producer.set<Text>(0, "hello");
  const auto want = producer.get<Q>(0);
  check(producer.export_cell<Q>(path, build, 0, input), "export a current cell");
  check(!producer.export_cell<Q>(path, build, 0, q<Text>{1}), "reject an incomplete dependency list");

  Db consumer;
  consumer.set<Text>(1, "allocate a different cell order");
  consumer.set<Text>(0, changed ? "a longer string" : "hello");
  check(!consumer.import_cell<Q>(path, Fp128{2, 1}, 0, input), "reject a different build");
  check(consumer.import_cell<Q>(path, build, 0, input), "import the cell into a fresh database");
  check(consumer.peek<Q>(0) == nullptr, "the cell is invisible before validation");
  const auto got = consumer.get<Q>(0);
  Db cold;
  cold.set<Text>(0, changed ? "a longer string" : "hello");
  check(got == cold.get<Q>(0), "imported result equals a cold run");
  check((got == want) != changed, "a changed input changes the value");
  check(consumer.runs<Q>(0) == (changed ? 1u : 0u), "the input fingerprint controls recomputation");
  check(consumer.stats().validated == (changed ? 0u : 1u), "the ordinary validator handles the record");

  consumer.set<Text>(0, "x");
  check(consumer.get<Q>(0) != got, "local revisions can invalidate a previously imported cell");
  check(consumer.runs<Q>(0) == (changed ? 2u : 1u), "a normal edit re-executes it");

  std::filesystem::remove(path);
  std::ofstream(path, std::ios::binary).write("truncated", 9);
  Db malformed;
  malformed.set<Text>(0, "hello");
  check(!malformed.import_cell<Q>(path, build, 0, input), "truncated record is ignored");
  check(malformed.get<Q>(0) == want && malformed.runs<Q>(0) == 1, "corrupt state falls back to a cold cell");
}

void paging(const std::string& dir) {
  auto check_db = [] {
    Db db;
    db.set<Text>(0, "abc");
    return db.get<Twice>(0) == 6;
  };
  if (setenv("YCXX_ENGINE_PAGING_DIR", dir.c_str(), 1) != 0) {
    throw std::runtime_error("cannot set paging directory");
  }
  {
    Db mapped;
    mapped.set<Text>(0, "abc");
    check(mapped.get<Twice>(0) == 6, "paged arenas preserve values");
#if defined(__linux__)
    std::ifstream maps("/proc/self/maps");
    if (maps) {
      std::string line;
      bool found = false;
      while (std::getline(maps, line)) {
        if (line.contains(dir + "/ycxx-arena.") && line.contains("(deleted)")) found = true;
      }
      check(found, "eligible arena storage has an unlinked backing file");
    }
#endif
  }
  if (setenv("YCXX_ENGINE_PAGING_DIR", (dir + "/missing").c_str(), 1) != 0) {
    throw std::runtime_error("cannot set an unavailable paging directory");
  }
  check(check_db(), "unavailable backing directory falls back to heap");
  unsetenv("YCXX_ENGINE_PAGING_DIR");
}

void run() {
  const auto directory = std::filesystem::temp_directory_path() /
                         std::format("ycxx-snapshot-{}", std::random_device{}());
  if (!std::filesystem::create_directory(directory)) throw std::runtime_error("snapshot test directory already exists");
  const auto dir = directory.native_encoded_string();
  const std::string path = dir + "/cache";
  round_trip<Length>(path, false);
  round_trip<Length>(path, true);
  round_trip<Twice>(path, false);
  round_trip<Twice>(path, true);
  paging(dir);
  std::filesystem::remove_all(dir);
}

}  // namespace ycxx::engine::snapshot_test

int main() {
  ycxx::engine::snapshot_test::run();
  return ycxx::unit::result("engine_snapshot");
}
