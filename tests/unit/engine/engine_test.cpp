// The query engine's core (#14): red/green validation with ordered groups, early cutoff,
// durability, dependency recording, diagnostics as values, demand lists, cycles, failures,
// serialization, and incremental == cold over random edits.
import std;
import ycxx.engine;
import ycxx.unit;

using namespace ycxx::engine;
using ycxx::unit::check;

namespace {

// Inputs: a project value, a system value, and a text.
struct [[=input{Durability::low}]] In {
  using Key = std::uint32_t;
  using Value = std::int64_t;
};
struct [[=input{Durability::high}]] Sys {
  using Key = std::uint32_t;
  using Value = std::int64_t;
};
struct [[=input{Durability::low}]] Text {
  using Key = std::uint32_t;
  using Value = std::string;
};

// A memoized copy of an input.
struct Echo {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return cx.fetch<In>(k); }
};

// Early cutoff: the length of a text, and something computed from the length only.
struct Length {
  using Key = std::uint32_t;
  using Value = std::size_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return cx.fetch<Text>(k).size(); }
};
struct LengthTimesTwo {
  using Key = std::uint32_t;
  using Value = std::size_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return 2 * co_await cx.fetch<Length>(k); }
};

// Groups: one parallel group, then a fetch that the new run may not make (§A1.5).
struct Grouped {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key) {
    const auto [a, b] = co_await cx.par_fetch(q<Echo>{0}, q<Echo>{1});
    if (a == 0) co_return b;
    co_return a + b + co_await cx.fetch<Echo>(2);
  }
};

// Durability: depends on a system input only.
struct FromSys {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return cx.fetch<Sys>(k) + 1; }
};
struct Mixed {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return co_await cx.fetch<FromSys>(k) + co_await cx.fetch<Echo>(k); }
};

// A cheap kind and a transparent helper.
struct [[=cheap]] Doubled {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static Value run(SyncCtx& cx, Key k) { return 2 * cx.fetch<In>(k); }
};
struct [[=transparent]] SumHelper {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static Value run(SyncCtx& cx, Key k) { return cx.fetch<In>(k) + cx.fetch<Doubled>(k); }
};
struct UsesHelper {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    // The same key twice: recorded once, at its first position.
    const auto x = cx.fetch<SumHelper>(k);
    const auto y = cx.fetch<In>(k);
    co_return x + y;
  }
};

// Diagnostics: a leaf that complains about negative inputs and returns the absolute value, and a
// parent whose value does not change when the leaf's diagnostic appears.
constexpr DiagId negative{1};
constexpr DiagId odd{2};
struct Leaf {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    const auto v = cx.fetch<In>(k);
    if (v < 0) cx.diag(Loc{k, 7}, negative, v);
    co_return v < 0 ? -v : v;
  }
};
struct Parent {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    const auto& v = co_await cx.par_map<Leaf>(std::array{k, k + 1});
    if (v[0].get() % 2 != 0) cx.diag(Loc{100, 0}, odd);
    co_return v[0].get() + v[1].get();
  }
};

// A demanded kind: its diagnostics belong to its demander's chain, not to `diags_of`.
struct [[=demanded]] Instantiate {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    cx.diag(Loc{k, 1}, negative);
    co_return cx.fetch<In>(k);
  }
};
struct Demander {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    const auto a = co_await cx.fetch<Echo>(k);
    const auto b = co_await cx.demand<Instantiate>(k, Loc{50, 3});
    co_return a + b;
  }
};

// A cycle when In(k) is nonzero.
struct Cyc {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    if (cx.fetch<In>(k) == 0) co_return 0;
    co_return 1 + co_await cx.fetch<Cyc>(1 - k);
  }
};

// A query that fails when In(k) is negative.
struct Throws {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    if (cx.fetch<In>(k) < 0) throw std::runtime_error("negative");
    co_return cx.fetch<In>(k);
  }
};

void basics() {
  Db db;
  db.set<In>(0, 5);
  check(db.get<Echo>(0) == 5, "a derived value");
  check(db.runs<Echo>(0) == 1, "executed once");
  check(db.get<Echo>(0) == 5 && db.runs<Echo>(0) == 1, "a second get is a hit");
  const Revision r = db.revision();
  db.set<In>(0, 5);
  check(db.revision() == r, "setting the same value starts no revision");
  db.set<In>(0, 6);
  db.set<In>(1, 1);
  check(db.revision() == r + 1, "one revision per batch of changes");
  check(db.get<Echo>(0) == 6 && db.runs<Echo>(0) == 2, "a changed input re-executes");
}

void early_cutoff() {
  Db db;
  db.set<Text>(0, "abc");
  check(db.get<LengthTimesTwo>(0) == 6, "cold");
  db.set<Text>(0, "xyz");
  db.reset_stats();
  check(db.get<LengthTimesTwo>(0) == 6, "same length");
  check(db.runs<Length>(0) == 2 && db.runs<LengthTimesTwo>(0) == 1, "the length re-ran and cut off its dependent");
  check(db.stats().backdated == 1 && db.stats().validated == 1, "one backdate, one green by dependencies");
  check(db.changed_at<Length>(0) == 1, "a backdated cell keeps its changed_at");
  db.set<Text>(0, "abcd");
  check(db.get<LengthTimesTwo>(0) == 8 && db.runs<LengthTimesTwo>(0) == 2, "a different length propagates");
}

void groups() {
  for (bool cold_first : {false, true}) {
    Db db;
    db.set<In>(0, 1);
    db.set<In>(1, 1);
    db.set<In>(2, 1);
    if (!cold_first) {
      check(db.get<Grouped>(0) == 3, "all three");
      const auto deps = db.dependencies<Grouped>(0);
      check(deps.size() == 2 && deps[0].size() == 2 && deps[1].size() == 1, "groups: [Echo 0, Echo 1], [Echo 2]");
      check(deps[0][0] == db.cell_of<Echo>(0) && deps[0][1] == db.cell_of<Echo>(1), "argument order");
    }
    db.set<In>(0, 0);
    db.set<In>(1, 5);
    db.set<In>(2, 7);
    check(db.get<Grouped>(0) == 5, "the new run skips Echo 2");
    if (!cold_first) {
      check(db.runs<Echo>(1) == 2, "every member of the changed group is verified (no short-circuit)");
      check(db.runs<Echo>(2) == 1, "the group after the changed one is not visited");
      check(db.dependencies<Grouped>(0).size() == 1, "the new dependency list");
    } else {
      check(db.runs<Echo>(2) == 0, "cold: Echo 2 is never demanded");
    }
  }
}

void durability() {
  Db db;
  db.set<Sys>(0, 10);
  db.set<In>(0, 1);
  check(db.get<Mixed>(0) == 12, "cold");
  db.set<In>(0, 2);
  db.reset_stats();
  check(db.get<FromSys>(0) == 11, "system-only cell");
  check(db.stats().durable == 1 && db.stats().validated == 0 && db.stats().executed == 0,
        "a high-durability cell is green in one comparison after a low-durability edit");
  check(db.get<Mixed>(0) == 13 && db.runs<Mixed>(0) == 2 && db.runs<FromSys>(0) == 1, "the low-durability path re-runs");
  db.set<Sys>(0, 20);
  check(db.get<Mixed>(0) == 23 && db.runs<FromSys>(0) == 2, "a high-durability edit invalidates everything");
}

// D reads the low input only when the high input says so; P is above D.
struct GatedLow {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    if (cx.fetch<Sys>(k) == 0) co_return 0;
    co_return cx.fetch<In>(k);
  }
};
struct AboveGated {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) { co_return 100 + co_await cx.fetch<GatedLow>(k); }
};

void durability_lowered_by_a_backdated_dependency() {
  Db db;
  db.set<Sys>(0, 0);
  db.set<In>(0, 0);
  check(db.get<AboveGated>(0) == 100, "cold: high durability");
  db.set<Sys>(0, 1);
  check(db.get<AboveGated>(0) == 100, "the dependency now reads a low input but is backdated");
  db.set<In>(0, 5);
  check(db.get<AboveGated>(0) == 105, "a cell validated green takes its dependencies' lowered durability");
}

void recording() {
  Db db;
  db.set<In>(3, 4);
  check(db.get<UsesHelper>(3) == 4 + 12, "a transparent helper and a cheap kind");
  check(db.get<Doubled>(3) == 8, "a cheap kind returns by value");
  const auto deps = db.dependencies<UsesHelper>(3);
  check(deps.size() == 2, "In once (deduplicated), then Doubled; the helper has no cell");
  check(deps.size() == 2 && deps[0][0] == db.cell_of<In>(3) && deps[1][0] == db.cell_of<Doubled>(3),
        "the helper's fetches land in the caller's list, in program order");
  db.set<In>(3, 5);
  check(db.get<UsesHelper>(3) == 5 + 15 && db.runs<Doubled>(3) == 2, "cheap kinds re-run like any other");
}

void diagnostics() {
  Db db;
  db.set<In>(0, 3);
  db.set<In>(1, 4);
  check(db.get<Parent>(0) == 7, "cold");
  {
    const auto& d = db.diags_of<Parent>(0);
    check(d.size() == 1 && d[0].id == odd, "the parent's own diagnostic");
  }
  // The leaf gains a diagnostic; its value is unchanged, so the parent re-runs and is backdated.
  db.set<In>(0, -3);
  check(db.get<Parent>(0) == 7 && db.runs<Parent>(0) == 2, "the parent re-ran");
  check(db.changed_at<Parent>(0) == 1, "and was backdated");
  {
    const auto& d = db.diags_of<Parent>(0);
    check(d.size() == 2 && d[0].id == odd && d[1].id == negative, "own first, then dependencies in program order");
    check(d.size() == 2 && d[1].at == Loc{0, 7} && d[1].args.size() == 1 && d[1].args[0].raw == static_cast<std::uint64_t>(-3),
          "location and arguments");
  }
  db.set<In>(0, 3);
  check(db.diags_of<Parent>(0).size() == 1, "the diagnostic goes away with its cause");
  db.set<In>(0, 4);
  check(db.diags_of<Parent>(0).empty(), "nothing to say");
  check(db.diags_of<Leaf>(1).empty(), "a cell without diagnostics");
}

// Always 0, so it is backdated whichever leaf it reads.
struct Switch {
  using Key = std::uint32_t;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    const std::uint32_t leaf = cx.fetch<In>(k + 10) > 0 ? k : k + 1;
    (void)co_await cx.fetch<Leaf>(leaf);
    co_return 0;
  }
};

void diagnostics_after_a_backdated_list_change() {
  Db db;
  db.set<In>(0, -1);
  db.set<In>(1, -1);  // both leaves have something to say, so has_diags does not flip
  db.set<In>(10, 1);
  check(db.diags_of<Switch>(0).size() == 1 && db.diags_of<Switch>(0)[0].at == Loc{0, 7}, "leaf 0's diagnostic");
  db.set<In>(10, 0);
  check(db.get<Switch>(0) == 0 && db.changed_at<Switch>(0) == 1, "backdated, now reading leaf 1; diags_of not asked");
  db.set<In>(1, -2);
  (void)db.get<Switch>(0);
  const auto& d = db.diags_of<Switch>(0);
  check(d.size() == 1 && d[0].at == Loc{1, 7} && d[0].args[0].raw == static_cast<std::uint64_t>(-2),
        "leaf 1's current diagnostic, not leaf 0's");
}

void demands() {
  Db db;
  db.set<In>(2, 9);
  check(db.get<Demander>(2) == 18, "cold");
  const auto list = db.demands<Demander>(2);
  check(list.size() == 1 && list[0].position == 1 && list[0].at == Loc{50, 3}, "instantiated(k, loc): k is the fetch's position");
  check(db.diags_of<Demander>(2).empty(), "diags_of does not descend into demanded kinds");
  check(db.diags_of<Instantiate>(2).size() == 1, "the demanded cell has its own");
}

void cycles() {
  std::string messages[2];
  for (std::uint32_t entry : {0u, 1u}) {
    Db db;
    db.set<In>(0, 1);
    db.set<In>(1, 1);
    bool caught = false;
    try {
      (void)db.get<Cyc>(entry);
    } catch (const query_cycle& e) {
      caught = true;
      messages[entry] = e.what();
      check(e.cells().size() == 2, "both cells are on the cycle");
    }
    check(caught, "a cycle is an internal error");
    check(db.live_frames() == 0, "every frame unwound");
    db.set<In>(1, 0);
    check(db.get<Cyc>(entry) == (entry == 0 ? 1 : 0), "the cells were reverted, and recover once the cycle is gone");
  }
  check(!messages[0].empty() && messages[0] == messages[1], "the report does not depend on the entry point");
  check(messages[0].starts_with("internal error: query cycle"), "the report");
}

void failures() {
  Db db;
  db.set<In>(0, -1);
  bool caught = false;
  try {
    (void)db.get<Throws>(0);
  } catch (const std::runtime_error&) {
    caught = true;
  }
  check(caught && db.live_frames() == 0, "an exception escaping a query reaches the request");
  db.set<In>(0, 2);
  check(db.get<Throws>(0) == 2 && db.runs<Throws>(0) == 1, "nothing was stored; the cell runs again");
  db.set<In>(0, -2);
  caught = false;
  try {
    (void)db.get<Throws>(0);
  } catch (const std::runtime_error&) {
    caught = true;
  }
  check(caught, "a failing re-execution");
  db.set<In>(0, 2);
  check(db.get<Throws>(0) == 2, "the old value was kept and is valid again");
}

struct Payload {
  std::uint32_t a;
  std::string s;
  std::vector<std::optional<std::int16_t>> xs;
  std::pair<bool, double> p;
  std::array<std::uint8_t, 3> arr;
  bool operator==(const Payload&) const = default;
};

void serialization() {
  const Payload p{7, "text", {std::nullopt, -3}, {true, 1.5}, {1, 2, 3}};
  Sink out;
  serialize(out, p);
  Source in(out.bytes());
  check(deserialize<Payload>(in) == p && in.done() && !in.failed(), "round trip");
  const auto bytes = out.bytes().first(out.bytes().size() - 1);
  Source cut(bytes);
  (void)deserialize<Payload>(cut);
  check(cut.failed(), "a truncated record is detected");
  const Diagnostic d{{1, 2}, odd, {diag_arg(5)}};
  Sink dout;
  serialize(dout, d);
  Source din(dout.bytes());
  check(deserialize<Diagnostic>(din) == d, "diagnostics serialize");
  static_assert(serializable<std::vector<Diagnostic>>);
  static_assert(!serializable<CellRef>, "a cell id is not storable");
  enum unscoped { u0, u1 };
  static_assert(!serializable<unscoped>, "corrupt input could hold an out-of-range value");
  Sink bad;
  bad.uint(std::uint8_t{2});
  Source bin(bad.bytes());
  (void)deserialize<bool>(bin);
  check(bin.failed(), "a bool is 0 or 1");
}

struct KeyA {
  std::uint32_t x;
  bool operator==(const KeyA&) const = default;
};
struct KeyB {
  std::uint32_t x;
  std::uint32_t y;
  bool operator==(const KeyB&) const = default;
};
struct KindA {
  using Key = KeyA;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx&, Key k) { co_return k.x; }
};
struct KindB {
  using Key = KeyB;
  using Value = std::int64_t;
  static query_task<Value> run(Ctx&, Key k) { co_return k.x; }
};

void concepts() {
  static_assert(InputKind<In> && !AsyncQuery<In>);
  static_assert(AsyncQuery<Echo> && !CheapQuery<Echo> && !DemandedQuery<Echo>);
  static_assert(CheapQuery<Doubled> && !AsyncQuery<Doubled>);
  static_assert(TransparentSync<SumHelper> && !MemoQuery<SumHelper>);
  static_assert(DemandedQuery<Instantiate>);
  static_assert(!QueryKey<std::string>, "keys are structural");
  static_assert(!QueryKey<int*>);
  static_assert(QueryKey<KeyB>);
  struct WithBase : KeyA {
    std::uint32_t y;
    bool operator==(const WithBase&) const = default;
  };
  static_assert(!QueryKey<WithBase>, "base members would be left out of the key");
  static_assert(kinds<KindA>::schema_hash != kinds<KindB>::schema_hash, "a key field changes the schema");
  static_assert(kinds<KindA, Echo>::schema_hash != kinds<Echo, KindA>::schema_hash, "kind ids are positions");
  check(kind_name<Echo>().contains("Echo"), "kind names come from reflection");
}

// incremental == cold over random edits (§A9): a layered program whose dependencies depend on
// input values, so edits change which cells exist and in which groups.
constexpr std::uint32_t layer = 12;
struct Node {
  using Key = std::uint32_t;  // level * layer + index
  using Value = std::int64_t;
  static query_task<Value> run(Ctx& cx, Key k) {
    const std::uint32_t level = k / layer, index = k % layer;
    // A durable corner whose dependency list depends on a system input (review of #37, P0).
    if (level == 0 && index % 4 == 0 && cx.fetch<Sys>(index) == 0) co_return 0;
    const auto own = cx.fetch<In>(index);
    if (level == 0) {
      if (own < 0) cx.diag(Loc{index, 0}, negative);
      co_return own;
    }
    const std::uint32_t below = (level - 1) * layer;
    const auto a = (index + static_cast<std::uint32_t>(own & 0xff)) % layer;
    if (own % 3 == 0) {
      const auto [x, y] = co_await cx.par_fetch(q<Node>{below + a}, q<Node>{below + (a + 1) % layer});
      co_return (x + y) % 7;  // small values, so cutoff happens often
    }
    const auto& xs = co_await cx.par_map<Node>(std::array{below + a, below + index});
    if (xs[0].get() == xs[1].get() && own % 2 == 0) co_return xs[0].get() % 7;
    co_return (xs[0].get() + co_await cx.fetch<Node>(below + (a + 2) % layer)) % 7;
  }
};

void incremental_equals_cold() {
  std::mt19937 rng(14);
  Db inc;
  std::vector<std::int64_t> values(layer);
  std::vector<std::int64_t> sys(layer);
  for (std::uint32_t i = 0; i < layer; ++i) inc.set<In>(i, values[i] = static_cast<std::int64_t>(rng() % 9) - 2);
  for (std::uint32_t i = 0; i < layer; ++i) inc.set<Sys>(i, sys[i] = 1);
  constexpr std::uint32_t top = 4 * layer;
  int mismatches = 0;
  const auto keys = [](const std::vector<std::vector<CellRef>>& groups) {
    std::vector<std::vector<ycxx::Fp128>> out;
    for (const auto& g : groups) out.push_back(g | std::views::transform(&CellRef::key) | std::ranges::to<std::vector>());
    return out;
  };
  for (int edit = 0; edit < 1000; ++edit) {
    for (int n = 0, edits = 1 + static_cast<int>(rng() % 3); n < edits; ++n) {
      const auto i = static_cast<std::uint32_t>(rng() % layer);
      if (rng() % 10 == 0) inc.set<Sys>(i, sys[i] = static_cast<std::int64_t>(rng() % 3));
      else inc.set<In>(i, values[i] = static_cast<std::int64_t>(rng() % 9) - 2);
    }
    Db cold;
    for (std::uint32_t i = 0; i < layer; ++i) cold.set<In>(i, values[i]);
    for (std::uint32_t i = 0; i < layer; ++i) cold.set<Sys>(i, sys[i]);
    for (std::uint32_t root = top; root < top + layer; ++root) {
      if (inc.get<Node>(root) != cold.get<Node>(root)) ++mismatches;
      if (inc.diags_of<Node>(root) != cold.diags_of<Node>(root)) ++mismatches;
    }
    // Every cell the cold run executed is valid in the incremental table: bringing it up to date
    // executes nothing (§A1.5, §A9).
    const Stats before = inc.stats();
    for (std::uint32_t k = 0; k < top + layer; ++k) {
      if (cold.runs<Node>(k) == 0) continue;
      if (inc.get<Node>(k) != cold.get<Node>(k)) ++mismatches;
      if (keys(inc.dependencies<Node>(k)) != keys(cold.dependencies<Node>(k))) ++mismatches;
    }
    if (inc.stats().executed != before.executed) ++mismatches;
  }
  check(mismatches == 0, std::format("incremental == cold over 1000 edits ({} mismatches)", mismatches));
  check(inc.stats().backdated > 0 && inc.stats().durable > 0 && inc.stats().validated > 0,
        "the edits exercised cutoff, the durability shortcut and validation");
}

}  // namespace

int main() {
  basics();
  early_cutoff();
  groups();
  durability();
  durability_lowered_by_a_backdated_dependency();
  recording();
  diagnostics();
  diagnostics_after_a_backdated_list_change();
  demands();
  cycles();
  failures();
  serialization();
  concepts();
  incremental_equals_cold();
  return ycxx::unit::result("engine");
}
