// The engine trace (#245): the stream of a small unit built cold, again, and after a whitespace
// edit, against a golden file (colors and times off); and the model behind the browser: the
// collector's batches, the tree's rows, filters and summary, and the timeline; and a live trace
// (#939), built as events come in any order, against the batches' tree.
//
//   unit_trace <golden>            checks
//   unit_trace <golden> --update   rewrites the golden file
import std;
import ycxx.engine;
import ycxx.execution;
import ycxx.pipeline;
import ycxx.pp;
import ycxx.trace;
import ycxx.unit;

namespace engine = ycxx::engine;
namespace pl = ycxx::pipeline;
namespace trace = ycxx::trace;
using trace::Event;
using trace::Outcome;
using ycxx::unit::check;

namespace {

constexpr pl::TuId tu{0};
constexpr pl::ProgramId program{0};

const std::string v0 = R"(int square(int x) { return x * x; }
int main() { return square(3) - 9; }
)";
const std::string v1 = R"(int square(int x) {
  return x  *  x;
}
int main() { return square(3) - 9; }
)";

// One build as the driver makes it, traced into `out`.
void build(pl::Session& s, const std::string& text, std::string_view title, std::ostream& out) {
  trace::Stream stream(out, {false, false});
  trace::Collector c([&](std::span<const Event> batch) { stream.batch(batch); });
  out << "## " << title << '\n';
  s.set_source(tu, "golden.cpp", text, "golden.o");
  (void)s.get<pl::TuFront>(tu, &c);
  (void)s.get<pl::Tu>(pl::Tu::Key{tu}, &c);
  s.set_program(program, {{tu}, {}, "golden"});
  const pl::Linked& l = s.get<pl::Link>(program, &c);
  check(l.errors.empty(), "the golden unit links");
  c.finish();
  stream.summary();
}

void golden(const std::string& path, bool update) {
  std::ostringstream got;
  pl::Session s;  // no pool: one thread, so the order is the same every run
  build(s, v0, "cold", got);
  build(s, v0, "again: the request's own cells are reused", got);
  build(s, v1, "a whitespace edit: pp runs, the front end cuts off, the rest is verified", got);
  // Fingerprints in keys (an optimization unit's) differ between hosts: the golden holds a mask.
  const std::string text = std::regex_replace(got.str(), std::regex(R"(\{[0-9a-f]{8},)"), "{<fp>,");
  // What the feature is for: the second build reuses everything and executes nothing.
  const std::size_t again = text.find("## again"), edit = text.find("## a whitespace");
  const std::string second = text.substr(again, edit - again);
  check(!second.contains(" executed"), "a second build executes nothing");
  check(second.contains(" reused"), "a second build shows reuse");
  const std::string third = text.substr(edit);
  check(third.contains(" cutoff") && third.contains(" verified"), "an edit shows early cutoff and verified cells");
  if (update) {
    std::ofstream(path, std::ios::binary) << text;
    std::println("updated {}", path);
    return;
  }
  std::ifstream in(path, std::ios::binary);
  const std::string want{std::istreambuf_iterator<char>(in), {}};
  check(text == want, "the stream equals the golden file (rerun with --update after a deliberate change)");
  if (text != want) std::println(std::cerr, "--- got:\n{}", text);
}

// A workspace query can demand the compiler in the same request (#1256).
struct WorkspaceBuildWitness {
  using Key = pl::RequestId;
  using Value = std::optional<ycxx::Fp128>;

  static engine::query_task<Value> run(engine::Ctx& cx, Key request) {
    const pl::Built& built = co_await cx.fetch<pl::Build>(request);
    if (built.units.size() != 1 || !built.units.front().object) co_return std::nullopt;
    co_return ycxx::fingerprint(*built.units.front().object);
  }
};

void nested_compiler() {
  constexpr std::string_view path = "trace-workspace.cpp";
  const auto source = ycxx::pp::path_id(path);
  engine::Pool pool(2);
  pl::Session session(pool);
  const auto request = session.request({.units = {tu}, .until = pl::Until::object});

  const auto set_source = [&](const std::string& text) {
    session.apply([&](engine::Db& db) {
      session.set_source(tu, std::string(path), text, "trace-workspace.o");
      db.set<ycxx::pp::FileContent>(source, text);
    });
  };
  const auto run = [&] {
    std::vector<Event> events;
    trace::Collector collector([&](std::span<const Event> batch) { events.append_range(batch); });
    const auto built = session.run<WorkspaceBuildWitness>(request, {}, nullptr, &collector);
    collector.finish();
    return std::pair{built.value_or(std::nullopt), std::move(events)};
  };
  const auto compiled_under_workspace = [](const std::vector<Event>& events) {
    const auto root = std::ranges::find_if(events, [](const Event& e) {
      return e.parent == 0 && e.kind == "WorkspaceBuildWitness" && e.outcome == Outcome::executed;
    });
    if (root == events.end()) return false;
    const auto build = std::ranges::find_if(events, [&](const Event& e) {
      return e.parent == root->id && e.kind == "Build" && e.outcome == Outcome::executed;
    });
    if (build == events.end()) return false;
    return std::ranges::any_of(events, [&](const Event& e) {
      return e.parent == build->id && e.kind == "Tu" && e.outcome == Outcome::executed;
    });
  };

  set_source("int main() { return 7; }\n");
  const auto [cold, cold_trace] = run();
  check(cold.has_value() && compiled_under_workspace(cold_trace),
        "the cold workspace trace contains the compiler's executed TU beneath the build");
  const auto [warm, warm_trace] = run();
  check(warm == cold && warm_trace.size() == 1 && warm_trace.front().kind == "WorkspaceBuildWitness" &&
            warm_trace.front().parent == 0 && warm_trace.front().outcome == Outcome::reused,
        "a warm workspace request traces the reused root without executing the compiler");
  set_source("int main() { return 8; }\n");
  const auto [edited, edited_trace] = run();
  check(edited.has_value() && edited != cold && compiled_under_workspace(edited_trace),
        "a changed source recompiles beneath the same workspace root");
}

struct [[=engine::input{engine::Durability::low}]] CancellationInput {
  using Key = int;
  using Value = int;
};

struct CancellationProbe {
  using Key = int;
  using Value = int;
  static engine::query_task<Value> run(engine::Ctx& cx, Key k) {
    co_await cx.stop_point();
    if (k == 1) throw std::runtime_error("query failed");
    co_return cx.fetch<CancellationInput>(k);
  }
  static std::string show(Value value) { return std::to_string(value); }
};

struct CancelAtStart : engine::Tracer {
  ycxx::exec::inplace_stop_source stop;
  trace::Live live;
  bool cancel = true;

  bool starts() const noexcept override { return true; }
  bool values() const noexcept override { return true; }
  void started(engine::TraceEvent event) noexcept override {
    if (cancel) stop.request_stop();
    live.started(std::move(event));
  }
  void event(engine::TraceEvent event) noexcept override { live.event(std::move(event)); }
};

void cancellation() {
  namespace ex = ycxx::exec;
  engine::Pool pool(1);
  engine::Db db(pool);
  auto cancel_query = [&](CancelAtStart& capture) {
    return ex::sync_wait(ex::write_env(
        db.request<CancellationProbe>(0, engine::Priority::interactive, nullptr, &capture),
        ex::prop(ex::get_stop_token, capture.stop.get_token())));
  };
  db.set<CancellationInput>(0, 7);
  CancelAtStart cold;
  check(!cancel_query(cold) && db.runs<CancellationProbe>(0) == 0, "a cold query stops in its body");
  check(std::ranges::any_of(cold.live.take(), [](const Event& e) { return !e.running && e.outcome == Outcome::stopped; }),
        "a cold cancellation reports stopped");
  check(db.get<CancellationProbe>(0) == 7, "the query initially publishes a value");
  db.set<CancellationInput>(0, 8);

  CancelAtStart capture;
  check(!cancel_query(capture) && db.runs<CancellationProbe>(0) == 1, "the cancelled query publishes nothing");
  auto events = capture.live.take();
  const auto terminal = std::ranges::find_if(events, [](const Event& e) { return !e.running && e.parent == 0; });
  check(terminal != events.end() && terminal->outcome == Outcome::stopped, "a cancelled query reports stopped");
  if (terminal != events.end()) {
    check(terminal->value.empty() && terminal->errors.empty() && terminal->retained_bytes == 0,
          "a cancelled recheck carries no old value or retained data");
    check(trace::json_line(*terminal).contains(R"("outcome":"stopped")"), "JSON identifies cancellation");
    trace::Tree tree;
    tree.set_filter("is:stopped");
    check(tree.matches(*terminal) && trace::state_name(*terminal) == "stopped", "the live browser identifies cancellation");
  }
  check(db.get<CancellationProbe>(0) == 8, "a retry after cancellation computes the updated value");

  capture.cancel = false;
  try {
    (void)ex::sync_wait(db.request<CancellationProbe>(1, engine::Priority::interactive, nullptr, &capture));
    check(false, "the failing query must throw");
  } catch (const std::runtime_error&) {
    check(std::ranges::any_of(capture.live.take(), [](const Event& e) { return !e.running && e.outcome == Outcome::failed; }),
          "an exception still reports failed");
  }
}

Event ev(std::uint64_t id, std::uint64_t parent, std::string kind, std::string key, Outcome o, std::uint64_t start = 0,
         std::uint64_t end = 0, std::int32_t worker = 0) {
  Event e;
  e.id = id;
  e.parent = parent;
  e.kind = std::move(kind);
  e.key = std::move(key);
  e.outcome = o;
  e.start_ns = start;
  e.end_ns = end;
  e.worker = e.end_worker = worker;
  return e;
}

engine::TraceEvent raw(std::uint64_t id, std::uint64_t parent, std::string_view kind, Outcome o) {
  engine::TraceEvent e;
  e.id = id;
  e.parent = parent;
  e.kind = kind;
  e.key = std::format("k{}", id);
  e.outcome = o;
  e.start = e.end = std::chrono::steady_clock::now();
  return e;
}

// Events arrive children first and from many workers; batches are whole subtrees in pre-order.
void collector() {
  std::vector<std::vector<std::pair<std::uint64_t, std::uint32_t>>> batches;
  trace::Collector c([&](std::span<const Event> b) {
    batches.emplace_back();
    for (const Event& e : b) batches.back().emplace_back(e.id, e.depth);
  });
  c.event(raw(3, 2, "C", Outcome::executed));
  c.event(raw(4, 2, "D", Outcome::reused));
  c.event(raw(2, 1, "B", Outcome::executed));
  c.event(raw(5, 1, "E", Outcome::input));
  check(batches.empty(), "nothing is handed on before its top-level event");
  c.event(raw(1, 0, "ycxx::pipeline::A", Outcome::executed));
  check(batches.size() == 1, "a top-level event hands on its subtree");
  using P = std::pair<std::uint64_t, std::uint32_t>;
  check(batches[0] == std::vector<P>{{1, 0}, {2, 1}, {3, 2}, {4, 2}, {5, 1}}, "pre-order, children by start, with depths");
  // A failed frame's child that was still running arrives after its parent.
  c.event(raw(6, 2, "F", Outcome::failed));
  check(batches.size() == 2 && batches[1] == std::vector<P>{{6, 2}}, "a late event goes on at its depth");
  // A parent that never arrives: its children go on at `finish`.
  c.event(raw(8, 7, "H", Outcome::reused));
  c.finish();
  check(batches.size() == 3 && batches[2] == std::vector<P>{{8, 0}}, "orphans go on at finish");

  // From many threads at once: every event exactly once, batches never interleaved.
  std::mutex m;
  std::vector<std::uint64_t> seen;
  bool overlapped = false;
  std::atomic<int> inside{0};
  trace::Collector many([&](std::span<const Event> b) {
    if (inside.fetch_add(1) != 0) overlapped = true;
    std::lock_guard g(m);
    for (const Event& e : b) seen.push_back(e.id);
    inside.fetch_sub(1);
  });
  std::vector<std::jthread> threads;
  for (std::uint64_t t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (std::uint64_t i = 0; i < 200; ++i) {
        const std::uint64_t top = 1 + t * 1000 + i * 3;
        many.event(raw(top + 1, top, "child", Outcome::reused));
        many.event(raw(top + 2, top, "child", Outcome::executed));
        many.event(raw(top, 0, "top", Outcome::executed));
      }
    });
  }
  threads.clear();
  check(seen.size() == 8 * 200 * 3, "every concurrent event is handed on");
  check(!overlapped, "batches never interleave");
  check(trace::short_kind("Tu@ycxx.pipeline") == "Tu" && trace::short_kind("ycxx::engine::DiagsOf") == "DiagsOf", "kind names are short");
}

std::vector<std::string> names(const trace::Tree& t) {
  std::vector<std::string> out;
  for (const auto& r : t.rows()) out.push_back(std::format("{}{}", std::string(r.depth, ' '), t.node(r.node).ev.key));
  return out;
}

void tree() {
  trace::Tree t;
  // Front:   a(executed, w0) > b(input, w1), c(executed, w1) > d(reused, w2) > e(cutoff, w2)
  std::vector<Event> batch = {
      ev(1, 0, "TuFront", "a.cpp", Outcome::executed, 0, 100, 0), ev(2, 1, "FileText", "a.cpp", Outcome::input, 5, 5, 1),
      ev(3, 1, "Pp", "a.cpp", Outcome::executed, 10, 60, 1),      ev(4, 3, "Region", "x.h", Outcome::reused, 20, 20, 2),
      ev(5, 4, "Deep", "y.h", Outcome::cutoff, 30, 40, 2),
  };
  batch[0].depth = 0, batch[1].depth = 1, batch[2].depth = 1, batch[3].depth = 2, batch[4].depth = 3;
  t.add(batch);
  // Depth below `open_depth` starts open: d (depth 2) is closed, so e is hidden.
  check(names(t) == std::vector<std::string>{"a.cpp", " a.cpp", " a.cpp", "  x.h"}, "rows follow expansion");
  t.toggle(3);
  check(names(t).size() == 5 && names(t).back() == "   y.h", "toggle opens a node");
  t.expand_all(false);
  check(names(t) == std::vector<std::string>{"a.cpp"}, "collapse all leaves the roots");
  t.reveal(4);
  check(names(t).size() == 5, "reveal opens the ancestors");
  t.toggle(3);
  t.toggle(3);

  // Filters: a match shows under its ancestors, whether they are open or not.
  t.expand_all(false);
  t.set_filter("y.h");
  check(names(t) == std::vector<std::string>{"a.cpp", " a.cpp", "  x.h", "   y.h"}, "a deep match shows its ancestors");
  check(t.rows().back().match && !t.rows().front().match, "ancestors are not matches");
  t.set_filter("REGION");
  check(t.rows().size() == 3 && t.node(t.rows()[2].node).ev.kind == "Region", "a kind matches without regard to case");
  t.set_filter("is:cut");
  check(t.rows().size() == 4 && t.node(t.rows().back().node).ev.outcome == Outcome::cutoff, "is: matches an outcome by prefix");
  t.set_filter("is:nothing");
  check(t.rows().empty(), "an unknown outcome matches nothing");
  t.set_filter("w:2 region");
  check(t.rows().size() == 3 && t.node(t.rows().back().node).ev.kind == "Region", "words and a worker together");
  t.set_filter("");
  t.set_worker(1);
  check(t.rows().size() == 3, "a worker filter keeps that worker's cells and their ancestors");
  t.set_worker(std::nullopt);
  check(!t.filtering(), "no filter");

  const trace::Summary s = t.summary();
  check(s.events == 5 && s.counts[std::to_underlying(Outcome::executed)] == 2 && s.counts[std::to_underlying(Outcome::input)] == 1,
        "the summary counts outcomes");
  check(s.span_ns == 100 && s.workers == 3, "the summary's span and workers");
  check(t.node(0).subtree == 5 && t.node(0).executions == 3 && t.node(2).executions == 2, "subtree counts");

  // The timeline: lane 0 is the requesting thread, lane k+1 worker k; the deepest cell wins.
  const trace::Timeline tl = trace::timeline(t, 10);
  check(tl.lanes.size() == 4 && tl.span_ns == 100, "one lane per worker and the requesting thread");
  check(tl.lanes[1][0] == 0 && tl.lanes[1][9] == 0, "a's span covers lane w0");
  check(tl.lanes[3][3] == 4, "the deepest cell in a slice wins");
  check(tl.lanes[2][1] == 2 && tl.lanes[2][0] == 1, "w1 reads the file, then runs pp");
  check(tl.lanes[0][5] == -1, "an idle slice");

  // Groups: a watched daemon's requests, each holding its top-level events.
  trace::Tree g;
  const std::uint64_t r1 = (std::uint64_t{1} << 63) | 1;
  g.open_group(r1, "#1 cxx a.cpp", 0);
  g.add(std::vector<Event>{ev(10, 0, "Tu", "a.cpp", Outcome::reused)}, r1);
  g.close_group(r1, 50);
  check(g.roots().size() == 1 && g.node(0).group && g.node(0).children.size() == 1, "a request holds its events");
  check(g.node(0).ev.outcome == Outcome::reused && g.node(0).ev.end_ns == 50, "a request that ran nothing is reused");
  check(g.summary().events == 1, "requests are not counted as events");
  const std::uint64_t r2 = (std::uint64_t{1} << 63) | 2;
  g.open_group(r2, "#2 cxx a.cpp", 60);
  g.add(std::vector<Event>{ev(20, 0, "Tu", "a.cpp", Outcome::executed)}, r2);
  check(!g.node(0).expanded && g.node(2).expanded, "a new request folds the earlier ones");
  check(g.node(g.rows().front().node).ev.key == "#1 cxx a.cpp", "oldest first by default");
  g.set_newest_first(true);
  check(g.node(g.rows().front().node).ev.key == "#2 cxx a.cpp" && g.rows().size() == 3, "newest first, the latest open");
  g.set_hide_reused(true);
  check(g.rows().size() == 3, "a reused request's node stays");
  g.set_expanded(0, true);
  check(g.rows().size() == 3, "its reused cells are hidden");
  g.set_hide_reused(false);
  check(g.rows().size() == 4, "and shown again");
}

void lines() {
  Event e = ev(1, 0, "Lower", "square", Outcome::cutoff, 1000, 2'500'000, 3);
  e.depth = 2;
  check(trace::stream_line(e, {false, false}) == "~     Lower(square) cutoff", "a plain line");
  check(trace::stream_line(e, {false, true}) == "~     Lower(square) cutoff 2.50ms w3", "a line with its time and worker");
  const std::string colored = trace::stream_line(e, {true, true});
  check(colored.contains("\x1b[33m") && colored.contains("◐"), "colors and glyphs on a terminal");
  e.end_worker = 5;
  check(trace::format_workers(e) == "w3→w5", "a frame that moved");
  check(trace::format_duration(999) == "999ns" && trace::format_duration(41'000) == "41µs" && trace::format_duration(3'400'000'000) == "3.40s",
        "durations");
  check(trace::worker_named("main") == -1 && trace::worker_named("w4") == 4 && trace::worker_named("4") == 4 && !trace::worker_named("x"),
        "worker names");
}

// Memory (#274): sums over subtrees count allocations everywhere and retained bytes only where a
// cell settles; the stream, the JSON line, the tree's order and the summary agree.
void memory() {
  const auto with = [](Event e, std::uint64_t alloc, std::int64_t retained, std::uint32_t depth) {
    e.alloc_bytes = alloc;
    e.allocs = alloc / 64;
    e.peak_bytes = static_cast<std::int64_t>(alloc);
    e.retained_bytes = retained;
    e.depth = depth;
    return e;
  };
  // Build → {Front → {Pp, Pp reused elsewhere}, Back}; the reuse retains the same bytes again.
  std::vector<Event> b = {
      with(ev(1, 0, "Build", "a", Outcome::executed, 0, 10), 100, 10, 0),
      with(ev(2, 1, "Front", "a", Outcome::executed, 1, 5), 1000, 300, 1),
      with(ev(3, 2, "Pp", "a", Outcome::executed, 2, 3), 5000, 2000, 2),
      with(ev(4, 1, "Back", "a", Outcome::executed, 6, 9), 3000, 50, 1),
      with(ev(5, 4, "Pp", "a", Outcome::reused, 7, 7), 0, 2000, 2),
  };
  b[2].cell = b[4].cell = 77;
  const std::vector<trace::Rollup> r = trace::rollups(b);
  check(r[0].alloc == 9100 && r[0].retained == 2360, std::format("the root's subtree ({}, {})", r[0].alloc, r[0].retained));
  check(r[1].alloc == 6000 && r[1].retained == 2300 && r[3].alloc == 3000 && r[3].retained == 50, "each subtree's");
  check(r[4].alloc == 0 && r[4].retained == 0, "a reuse counts nothing: its bytes count where the cell settled");

  check(trace::format_bytes(812) == "812B" && trace::format_bytes(41 * 1024) == "41.0KiB" && trace::format_bytes(1300000) == "1.24MiB" &&
            trace::format_bytes(-2048) == "-2.0KiB",
        "bytes");
  const std::string line = trace::stream_line(b[1], {false, true}, false, {}, r[1]);
  check(line.ends_with(" alloc 1000B, 5.9KiB in all, retains 300B"), std::format("a line's memory: {}", line));
  check(!trace::stream_line(b[1], {false, false}, false, {}, r[1]).contains("alloc"), "left out with times, as the golden has it");
  check(!trace::stream_line(b[4], {false, true}, false, {}, r[4]).contains("retains"), "a reuse shows no retained bytes");
  const std::string json = trace::json_line(b[1], false, std::nullopt, {}, r[1]);
  check(json.contains(R"("alloc_bytes":1000,"allocs":15,"peak_bytes":1000,"retained_bytes":300,"incl_alloc_bytes":6000,"incl_retained_bytes":2300)"),
        std::format("the JSON line's memory: {}", json));

  // The tree: inclusive sums, and siblings by memory.
  trace::Tree t;
  t.add(b);
  const auto names = [&] {
    std::vector<std::string> out;
    for (const auto& row : t.rows()) out.push_back(t.node(row.node).ev.kind);
    return out;
  };
  t.expand_all(true);
  check(t.node(0).incl.alloc == 9100 && t.node(0).incl.retained == 2360, "the tree's sums are the stream's");
  check(names() == std::vector<std::string>{"Build", "Front", "Pp", "Back", "Pp"}, "start order");
  t.set_order(trace::Tree::Order::alloc);
  check(names() == std::vector<std::string>{"Build", "Front", "Pp", "Back", "Pp"}, "by bytes allocated: Front's 6000 before Back's 3000");
  t.set_order(trace::Tree::Order::retained);
  check(names() == std::vector<std::string>{"Build", "Front", "Pp", "Back", "Pp"}, "by bytes retained: Front's 2300 before Back's 50");
  b[3].alloc_bytes = 7000;  // Back now allocates more than Front's subtree
  trace::Tree t2;
  t2.add(b);
  t2.expand_all(true);
  t2.set_order(trace::Tree::Order::alloc);
  std::vector<std::string> got;
  for (const auto& row : t2.rows()) got.push_back(t2.node(row.node).ev.kind);
  check(got == std::vector<std::string>{"Build", "Back", "Pp", "Front", "Pp"}, "most allocated first");
  // Retained order differs from both: Back allocates less than Front's subtree but retains most.
  b[3].alloc_bytes = 3000;
  b[3].retained_bytes = 5000;
  trace::Tree t3;
  t3.add(b);
  t3.expand_all(true);
  t3.set_order(trace::Tree::Order::retained);
  got.clear();
  for (const auto& row : t3.rows()) got.push_back(t3.node(row.node).ev.kind);
  check(got == std::vector<std::string>{"Build", "Back", "Pp", "Front", "Pp"}, "most retained first");
  t3.set_order(trace::Tree::Order::alloc);
  got.clear();
  for (const auto& row : t3.rows()) got.push_back(t3.node(row.node).ev.kind);
  check(got == std::vector<std::string>{"Build", "Front", "Pp", "Back", "Pp"}, "and by bytes allocated, Front first");
  b[3].alloc_bytes = 7000;
  b[3].retained_bytes = 50;
  const trace::Summary s = t.summary();
  check(s.alloc_bytes == 9100 && s.retained_bytes == 2360, "the summary's memory");

  // The stream's summary: the build's memory and its kinds.
  std::ostringstream out;
  trace::Stream st(out, {false, true});
  st.batch(b);
  st.summary(1000, trace::BuildMemory{4096, 8192});
  const std::string text = out.str();
  check(text.contains("memory: 12.8KiB allocated in 140 allocations, 2.3KiB retained; process 4.0KiB, peak 8.0KiB"),
        std::format("the memory line: {}", text));
  check(text.contains("by kind: Back 6.8KiB, Pp 4.9KiB, Front 1000B, Build 100B"), "kinds by bytes allocated");
  std::ostringstream quiet;
  trace::Stream golden_style(quiet, {false, false});
  golden_style.batch(b);
  golden_style.summary();
  check(!quiet.str().contains("memory"), "no memory lines when times are off");
  std::ostringstream js;
  trace::Stream jst(js, {false, true, true});
  jst.batch(b);
  jst.summary(1000, trace::BuildMemory{4096, 8192});
  check(js.str().contains(R"("memory":{"alloc_bytes":13100,"allocs":140,"retained_bytes":2360,"process_current_bytes":4096,"process_peak_bytes":8192},"kinds":[{"kind":"Back")"),
        std::format("the JSON summary: {}", js.str().substr(js.str().rfind("{\"summary"))));
}

// ---- the browser's model (#635): frames, hit-testing and input sequences

// A trace with something in every column: two units' worth of cells, an error, a long value.
std::vector<Event> browser_events() {
  const auto with = [](Event e, std::uint32_t depth, std::uint64_t alloc = 0, std::int64_t retained = 0) {
    e.depth = depth;
    e.alloc_bytes = alloc;
    e.allocs = alloc / 32;
    e.peak_bytes = static_cast<std::int64_t>(alloc);
    e.retained_bytes = retained;
    return e;
  };
  std::vector<Event> b = {
      with(ev(1, 0, "Tu", "a.cpp", Outcome::executed, 0, 900'000, 0), 0, 4096, 1024),
      with(ev(2, 1, "Pp", "a.cpp", Outcome::executed, 10'000, 200'000, 1), 1, 20'480, 5120),
      with(ev(3, 2, "FileText", "a.cpp", Outcome::input, 10'000, 10'000, 1), 2, 0, 300),
      with(ev(4, 2, "FileText", "x.h", Outcome::input, 11'000, 11'000, 1), 2, 0, 200),
      with(ev(5, 1, "FunctionIr", "square", Outcome::executed, 200'000, 500'000, 2), 1, 1000, 300),
      with(ev(6, 5, "FunctionGen", "square", Outcome::executed, 210'000, 400'000, 2), 2, 3000, 0),
      with(ev(7, 1, "Codegen", "main", Outcome::executed, 500'000, 850'000, 0), 1, 500, 100),
      with(ev(8, 7, "Optimized", "{1, aarch64_macos, 0}", Outcome::executed, 600'000, 840'000, 3), 2, 700, 64),
      with(ev(9, 1, "SourceOf", "a.cpp", Outcome::reused, 860'000, 860'000, 0), 1),
  };
  b[4].value = "func {\n";
  for (int i = 0; i < 30; ++i) b[4].value += std::format("  %{} = const.i32 {}\n", i, i * i);
  b[4].value += "}";
  b[7].errors = "a.cpp:2:5: error: no instruction for this";
  b[6].errors = b[7].errors;
  b[8].repeat = 3;
  return b;
}

std::string plain(const trace::Frame& f) {
  std::string out;
  for (const std::string& l : f.lines) {
    std::string p = trace::strip_sgr(l);
    while (p.ends_with(' ')) p.pop_back();
    out += p + '\n';
  }
  return out;
}

trace::Mouse click(std::size_t x, std::size_t y, trace::Mouse::Act act = trace::Mouse::Act::press) {
  trace::Mouse m;
  m.act = act;
  m.x = x;
  m.y = y;
  return m;
}

void input() {
  using trace::Mouse;
  const auto mouse = [](std::string_view s) { return std::get<Mouse>(trace::parse_input(s)); };
  check(mouse("\x1b[<0;12;5M") == click(11, 4), "a left press, 1-based on the wire");
  check(mouse("\x1b[<0;12;5m").act == Mouse::Act::release, "its release");
  check(mouse("\x1b[<32;3;4M") == click(2, 3, Mouse::Act::drag), "motion with the left button down is a drag");
  check(mouse("\x1b[<64;1;1M").act == Mouse::Act::wheel_up && mouse("\x1b[<65;1;1M").act == Mouse::Act::wheel_down, "the wheel");
  check(mouse("\x1b[<69;1;1M").shift && mouse("\x1b[<69;1;1M").act == Mouse::Act::wheel_down, "shift with the wheel");
  check(mouse("\x1b[<2;300;100M").button == 2 && mouse("\x1b[<2;300;100M").x == 299, "a right press past column 223");
  check(mouse("\x1b[<35;5;5M").act == Mouse::Act::move, "motion with no button");
  // Legacy reports (no 1006): one input each, bytes 32 above their values.
  check(std::get<Mouse>(trace::parse_input("\x1b[M !\"")) == click(0, 1), "a legacy left press at column 0, row 1");
  check(std::get<Mouse>(trace::parse_input("\x1b[M#qq")).act == Mouse::Act::release, "a legacy release");
  check(std::get<Mouse>(trace::parse_input("\x1b[Ma!!")).act == Mouse::Act::wheel_down, "a legacy wheel");
  // Horizontal wheels and extra buttons do nothing (keys no binding has).
  for (std::string_view k : {"\x1b[<66;5;5M", "\x1b[<67;5;5M", "\x1b[<128;5;5M", "\x1b[<129;5;5M"})
    check(std::holds_alternative<trace::Key>(trace::parse_input(k)), std::format("not a click or a scroll: {:?}", k));
  for (std::string_view k : {"q", "\x1b[A", "\x1b", "\x1b[<0;0;5M", "\x1b[<0;5M", "\x1b[<a;1;1M"})
    check(std::get<trace::Key>(trace::parse_input(k)).seq == k, std::format("a key, or a malformed report as one: {:?}", k));
}

// A scripted session: each step's frame (plain, 24×120) against `browser.golden`; and the hits.
void browser(const std::filesystem::path& golden_path, bool update) {
  using H = trace::Hit;
  trace::Tree t;
  t.add(browser_events());
  trace::Browser b(t);
  b.resize(24, 120);
  std::string got;
  const auto step = [&](std::string_view name) {
    got += std::format("## {}\n", name);
    got += plain(b.frame());
  };
  const auto press = [&](std::size_t x, std::size_t y, std::chrono::milliseconds at = std::chrono::milliseconds{0}) {
    (void)b.handle(click(x, y), at);
    (void)b.handle(click(x, y, trace::Mouse::Act::release), at);
  };
  using std::chrono::milliseconds;
  step("start");
  // Rows: 0 the title bar, 1 the summary, 2 the toggles, 3 column headers, 4.. the tree; the
  // divider at 72 (3/5 of 120).
  check(b.left_width() == 72, "the tree takes 3/5");
  check(b.hit(0, 0) == H{H::Area::header}, "the header");
  check(b.hit(14, 0) == H{H::Area::tab, std::to_underlying(trace::View::tree)}, "the tree's tab, in the title bar");
  check(b.hit(20, 0) == H{H::Area::tab, std::to_underlying(trace::View::timeline)}, "the timeline's tab");
  check(b.hit(5, 0) == H{H::Area::header}, "the title");
  check(b.hit(1, 2) == H{H::Area::toggle, std::to_underlying(trace::Toggle::filter)}, "the filter's chip");
  check(b.hit(12, 2) == H{H::Area::toggle, std::to_underlying(trace::Toggle::worker)}, "the worker's chip");
  check(b.hit(0, 2) == H{H::Area::status} && b.hit(10, 2) == H{H::Area::status}, "between the chips");
  check(b.hit(1, 3) == H{H::Area::column, std::to_underlying(trace::Column::cell)}, "the cell's header");
  check(b.hit(40, 3) == H{H::Area::column, std::to_underlying(trace::Column::time)}, "the time header");
  check(b.hit(71, 3) == H{H::Area::column, std::to_underlying(trace::Column::retained)}, "the retains header");
  check(b.hit(0, 4) == H{H::Area::marker, 0} && b.hit(5, 4) == H{H::Area::row, 0}, "the first row's marker, and the row");
  check(b.hit(2, 5) == H{H::Area::marker, 1} && b.hit(0, 5) == H{H::Area::row, 1}, "a nested row's marker is indented");
  check(b.hit(2, 6) == H{H::Area::row, 2}, "a leaf has no marker");
  check(b.hit(5, 21) == H{}, "below the last row");
  check(b.hit(72, 11) == H{H::Area::divider} && b.hit(73, 11) == H{H::Area::divider} && b.hit(71, 11) != H{H::Area::divider}, "the divider, two columns wide to grab");
  check(b.hit(90, 3) == H{H::Area::title} && b.hit(90, 4) == H{H::Area::detail, 0} && b.hit(72, 3) == H{H::Area::divider}, "the pane's title and first line; the divider in the title row");
  check(b.hit(5, 23) == H{H::Area::footer} && b.hit(5, 22) == H{}, "the footer, on the last row");

  press(2, 8);  // FunctionIr(square)'s marker folds it (depth 1 starts open)
  step("a click on a marker folds the row, and selects it");
  check(b.cursor() == 4 && !t.node(4).expanded, "selected and folded");
  press(2, 8);
  check(t.node(4).expanded, "a second click opens it again");
  press(2, 8);
  press(10, 5, milliseconds{1000});
  press(10, 5, milliseconds{1200});  // a double click folds Pp
  step("a double click folds a row");
  check(b.cursor() == 1 && !t.node(1).expanded, "Pp is folded");
  press(10, 5, milliseconds{2000});
  press(10, 5, milliseconds{2500});  // too slow: two clicks
  check(!t.node(1).expanded, "two slow clicks fold nothing");
  press(40, 3);
  step("a click on the time header sorts by time");
  check(t.order() == trace::Tree::Order::time, "by time");
  press(40, 3);
  check(t.order() == trace::Tree::Order::start, "a second click goes back to start order");
  // Columns: cell 0–34, time 36–43, worker 45–51, alloc 53–61, retains 63–71.
  press(58, 3);
  check(t.order() == trace::Tree::Order::alloc, "the alloc header");
  press(66, 3);
  check(t.order() == trace::Tree::Order::retained, "the retains header");
  press(58, 3);
  press(48, 3);
  check(t.order() == trace::Tree::Order::alloc, "the worker header does not sort");
  press(1, 3);
  check(t.order() == trace::Tree::Order::start, "the cell header: start order");

  // The pane: a click focuses it, the wheel scrolls it, keys then scroll it.
  press(10, 6);  // FunctionIr(square), whose value is long
  press(90, 9);
  check(b.focus() == trace::Focus::detail, "a click on the pane focuses it");
  (void)b.handle(click(90, 9, trace::Mouse::Act::wheel_down));
  (void)b.handle(click(90, 9, trace::Mouse::Act::wheel_down));
  step("the wheel scrolls the pane under it");
  check(b.detail_scroll() == 2 * trace::Browser::wheel_step, "two notches");
  (void)b.handle(trace::Key{"j"});
  check(b.detail_scroll() == 2 * trace::Browser::wheel_step + 1 && b.cursor() == 2, "j scrolls the focused pane, not the tree");
  (void)b.handle(trace::Key{"\x1b"});
  (void)b.handle(trace::Key{"j"});
  check(b.focus() == trace::Focus::tree && b.cursor() == 3, "Escape gives the keys back to the tree");

  // The divider drags, within bounds.
  (void)b.handle(click(72, 11));
  check(b.dragging(), "a press on the divider starts a drag");
  (void)b.handle(click(60, 11, trace::Mouse::Act::drag));
  (void)b.handle(click(60, 11, trace::Mouse::Act::release));
  step("the divider dragged to column 60");
  check(b.left_width() == 60 && !b.dragging(), "the tree is 60 wide");
  (void)b.handle(click(74, 11));
  check(!b.dragging(), "the divider has moved: column 74 is the pane");
  (void)b.handle(click(60, 11));
  (void)b.handle(click(2, 11, trace::Mouse::Act::drag));
  (void)b.handle(click(2, 11, trace::Mouse::Act::release));
  check(b.left_width() == 24, "a pane keeps 24 columns");
  check(b.hit(23, 3) == H{H::Area::column, std::to_underlying(trace::Column::cell)}, "a narrow tree drops its columns: the cell's is all");
  // < and > move the divider; it keeps its share of the width when the terminal is resized.
  (void)b.handle(trace::Key{">"});
  (void)b.handle(trace::Key{">"});
  check(b.left_width() == 32, std::format("> moves the divider right: {}", b.left_width()));
  (void)b.handle(trace::Key{"<"});
  check(b.left_width() == 28, "< moves it left");
  for (int i = 0; i < 40; ++i) (void)b.handle(trace::Key{">"});
  check(b.left_width() == 96, "the pane keeps 24 columns");
  b.resize(24, 200);
  check(b.left_width() == 160, std::format("a wider terminal keeps the share (4/5): {}", b.left_width()));
  b.resize(24, 120);
  for (int i = 0; i < 6; ++i) (void)b.handle(trace::Key{"<"});
  check(b.left_width() == 72, "back to 72");

  // The wheel over the tree moves the view and keeps the selection in it.
  t.expand_all(true);
  b.resize(10, 120);  // 6 tree rows of 9
  (void)b.frame();
  press(5, 4);  // the tree takes the keys again (the last click was on the pane)
  (void)b.handle(trace::Key{"g"});
  (void)b.handle(click(5, 6, trace::Mouse::Act::wheel_down));
  (void)b.frame();
  check(b.top() == 3 && b.cursor() == 3, std::format("the view moved 3 rows, the selection with it ({} {})", b.top(), b.cursor()));
  (void)b.handle(click(5, 6, trace::Mouse::Act::wheel_down));
  (void)b.handle(click(5, 6, trace::Mouse::Act::wheel_down));
  (void)b.frame();
  check(b.top() == 4, "and stops at the last page (5 rows of 9)");
  (void)b.handle(click(5, 6, trace::Mouse::Act::wheel_up));
  (void)b.frame();
  check(b.top() == 1 && b.cursor() == 4, "up again; the selection stays where it was (clamped to the last page), in view");
  b.resize(24, 120);

  // A narrow screen: the summary wraps onto more lines rather than run off, and everything
  // below moves down with it.
  b.resize(24, 60);
  step("a narrow screen: the summary wraps");
  check(b.header_height() == 5, std::format("three summary lines at 60 columns, the title bar and the toggles: {}", b.header_height()));
  check(b.hit(14, 0) == H{H::Area::tab, std::to_underlying(trace::View::tree)} && b.hit(1, 4) == H{H::Area::toggle, 0}, "the toggles move down");
  check(b.hit(5, 6) == H{H::Area::row, 0}, "and the rows");
  for (const std::string& l : b.frame().lines) check(trace::columns(l) == 60, "every line is the screen's width");
  b.resize(24, 120);
  check(b.header_height() == 3, "one summary line at 120");

  // The timeline: its tab, a click on a slice, a double click opens it in the tree.
  press(20, 0);
  check(b.view() == trace::View::timeline, "the timeline's tab");
  step("the timeline");
  // Lanes from row 3 (row 2 the axis): main, w0, w1, w2, w3; slices from column 9.
  check(b.hit(20, 7) == H{H::Area::lane, 3, 11}, "a slice of w2's lane");
  press(9 + 30, 7, milliseconds{5000});
  check(b.lane() == 3 && b.time() == 30, "a click moves the cursor there");
  (void)b.handle(click(9 + 40, 7, trace::Mouse::Act::wheel_down));
  check(b.time() == 31, "the wheel scrubs");
  press(9 + 30, 7, milliseconds{6000});
  press(9 + 30, 7, milliseconds{6100});
  check(b.view() == trace::View::tree, "a double click opens the slice in the tree");
  check(t.node(t.rows()[b.cursor()].node).ev.kind == "FunctionGen", "at the cell w2 was in");
  press(14, 0);
  check(b.view() == trace::View::tree, "the tree's tab");

  // Review fixes: a double click is twice on the same thing; a lost release ends no drag; the pane
  // gives the keys back when it is hidden; a marker cut off by the columns is not a target.
  (void)b.handle(trace::Key{"t"});
  press(9 + 10, 7, milliseconds{7000});
  press(9 + 50, 7, milliseconds{7100});
  check(b.view() == trace::View::timeline && b.time() == 50, "two clicks on different slices of a lane are no double click");
  (void)b.handle(trace::Key{"t"});
  (void)b.handle(click(72, 11));  // a press on the divider whose release is lost
  (void)b.handle(click(10, 6));   // the next press, on a row
  (void)b.handle(click(30, 6, trace::Mouse::Act::drag));
  check(b.left_width() == 72 && !b.dragging(), "a lost release: the next press ends the drag");
  press(90, 9);
  check(b.focus() == trace::Focus::detail, "the pane has the keys");
  b.resize(24, 90);
  const std::size_t at = b.cursor();
  (void)b.handle(trace::Key{"j"});
  check(b.focus() == trace::Focus::tree && b.cursor() == at + 1, "a hidden pane gives the keys back to the tree");
  b.resize(24, 120);
  press(5, 4);

  // Help closes on a click; a click closes the filter prompt and keeps what was typed.
  (void)b.handle(trace::Key{"?"});
  press(5, 6);
  check(b.frame().lines[3].find("What the engine") == std::string::npos, "a click closes the help");
  (void)b.handle(trace::Key{"/"});
  for (char c : std::string_view("functiong")) (void)b.handle(trace::Key{std::string(1, c)});
  press(5, 4);
  check(t.filtering() && t.rows().size() == 3, std::format("a click ends the prompt, the filter stays (Tu > FunctionIr > FunctionGen): {}", t.rows().size()));
  check(!b.frame().cursor, "and the cursor is hidden again");

  // A row deep enough that its marker is cut off by the columns (cell column 0–34 at 120): no target.
  {
    trace::Tree deep;
    std::vector<Event> chain;
    for (std::uint32_t d = 0; d < 20; ++d) {
      chain.push_back(ev(d + 1, d, "K", std::format("{}", d), Outcome::executed));
      chain.back().depth = d;
    }
    deep.add(chain);
    deep.expand_all(true);
    trace::Browser db(deep);
    db.resize(30, 120);
    (void)db.frame();
    check(db.hit(32, 3 + 1 + 16) == H{H::Area::marker, 16}, "depth 16: the marker fits (32–33)");
    check(db.hit(34, 3 + 1 + 17) == H{H::Area::row, 17}, "depth 17: the marker (34–35) is cut off, so the row");
    check(db.hit(36, 3 + 1 + 18) == H{H::Area::row, 18}, "depth 18: a click in the time column is the row");
  }

  const std::string text = got;
  if (update) {
    std::ofstream(golden_path, std::ios::binary) << text;
    std::println("updated {}", golden_path.native());
    return;
  }
  std::ifstream in(golden_path, std::ios::binary);
  const std::string want{std::istreambuf_iterator<char>(in), {}};
  check(text == want, "the browser's frames equal browser.golden (rerun with --update after a deliberate change)");
  if (text != want) std::println(std::cerr, "--- got:\n{}", text);
}

// Selection and copy (#636): what each way of copying puts on the clipboard, exactly.
void copying() {
  // The native clipboard (macOS: NSPasteboard), on a pasteboard of the test's own: the user's
  // clipboard is never touched. Elsewhere there is none yet (#730).
  // PARITY: #730: Linux runs a clipboard command until it has a native clipboard.
  // Over SSH a Mac may have no logged-in session, hence no pasteboard to reach (the browser does
  // not use it there either).
  const bool session = !std::getenv("SSH_CONNECTION") && !std::getenv("SSH_TTY");
  if (!trace::native_clipboard.empty() && !session) {
    std::println("unit.trace: over SSH, the pasteboard's checks are skipped");
  } else if (!trace::native_clipboard.empty()) {
    const std::string name = std::format("org.ycxx.unit-trace.{}", std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string text = "Lower(square) cutoff 2.50ms\n  %0 = const.i32 0 — héllo ✓";
    check(trace::pasteboard_write(text, name), "the pasteboard takes the text");
    check(trace::pasteboard_read(name) == text, std::format("and gives it back, UTF-8 and all: {:?}", trace::pasteboard_read(name).value_or("(none)")));
    check(trace::pasteboard_write("second", name) && trace::pasteboard_read(name) == "second", "a second copy replaces the first");
    check(!trace::pasteboard_write(std::string_view("\xff\xfe", 2), name) && trace::pasteboard_read(name) == "second",
          "bytes that are not UTF-8 are refused, and what it held stays");
    trace::pasteboard_drop(name);
  } else {
    check(!trace::pasteboard_write("x", "y") && !trace::pasteboard_read("y"), "no native clipboard here");
  }

  check(trace::base64("") == "" && trace::base64("f") == "Zg==" && trace::base64("fo") == "Zm8=" && trace::base64("foo") == "Zm9v" &&
            trace::base64("foobar") == "Zm9vYmFy",
        "base64 (RFC 4648's examples)");
  check(trace::osc52("hi", false) == "\x1b]52;c;aGk=\x07", "OSC 52");
  check(trace::osc52("hi", true) == "\x1b]52;c;aGk=\x07\x1bPtmux;\x1b\x1b]52;c;aGk=\x07\x1b\\", "and tmux's passthrough form");
  check(trace::slice_columns("héllo", 1, 3) == "él" && trace::slice_columns("ab", 1, 9) == "b" && trace::slice_columns("ab", 5, 9).empty(),
        "columns of a line");
  check(trace::strip_sgr(trace::highlight("\x1b[1mabc\x1b[0mdef", 2, 4)) == "abcdef", "a highlight keeps the text");
  check(trace::highlight("abcd", 1, 3) == "a\x1b[7mbc\x1b[27md", "and marks the columns");

  trace::Tree t;
  t.add(browser_events());
  trace::Browser b(t);
  b.resize(24, 120);
  (void)b.frame();
  using M = trace::Mouse::Act;
  std::chrono::milliseconds clock{0};  // a second apart: no double clicks
  const auto act = [&](std::size_t x, std::size_t y, M a) { return b.handle(click(x, y, a), clock += std::chrono::seconds(1)); };
  const auto key = [&](std::string k) { return b.handle(trace::Key{std::move(k)}); };

  // Rows: 0 Tu, 1 Pp, 2–3 FileText, 4 FunctionIr, 5 FunctionGen, 6 Codegen, 7 Optimized, 8 SourceOf;
  // row r on screen line 4 + r.
  (void)act(10, 5, M::press);
  (void)act(10, 6, M::drag);
  (void)act(10, 7, M::drag);
  trace::Action a = act(10, 7, M::release);
  check(a.copy == "* Pp(a.cpp) executed 190µs w1 alloc 20.0KiB, retains 5.0KiB\n"
                  ".   FileText(a.cpp) input w1 retains 300B\n"
                  ".   FileText(x.h) input w1 retains 200B\n",
        std::format("a drag over three rows copies them as the stream prints them: {:?}", a.copy.value_or("(nothing)")));
  check(trace::strip_sgr(b.frame().lines.back()).contains("copied 3 rows"), "the status line says so");
  (void)act(10, 10, M::press);
  a = act(10, 10, M::release);
  check(!a.copy && b.cursor() == 6, "a click copies nothing");
  check(!trace::strip_sgr(b.frame().lines.back()).contains("copied"), "and the note goes");
  (void)act(10, 10, M::press);  // Codegen, whose error starts below it, and Optimized, where it starts
  (void)act(10, 11, M::drag);
  a = act(10, 11, M::release);
  check(a.copy == "* Codegen(main) executed 350µs w0 alloc 500B, 1.2KiB in all, retains 100B (error below: a.cpp:2:5: error: no instruction for this)\n"
                  "*   Optimized({1, aarch64_macos, 0}) executed 240µs w3 alloc 700B, retains 64B ← error here\n"
                  "      error: a.cpp:2:5: error: no instruction for this\n",
        std::format("errors copy as the stream shows them: {:?}", a.copy.value_or("(nothing)")));

  // The pane: a drag over the value's text copies those columns. Its lines start at screen line 4;
  // FunctionIr's value starts at pane line 21 (`func {`); the wheel scrolls 6 lines down.
  (void)act(10, 8, M::press);
  (void)act(10, 8, M::release);
  (void)act(90, 11, M::wheel_down);
  (void)act(90, 11, M::wheel_down);
  (void)b.frame();
  check(b.detail_scroll() == 6, "scrolled");
  (void)act(73 + 3, 4 + 22 - 6, M::press);
  (void)act(73 + 6, 4 + 23 - 6, M::drag);
  a = act(73 + 6, 4 + 23 - 6, M::release);
  check(a.copy == "%0 = const.i32 0\n  %1 =", std::format("a drag over the pane copies the text between its ends: {:?}", a.copy.value_or("(nothing)")));
  check(b.focus() == trace::Focus::detail, "and gives the pane the keys");
  // Backwards, and from the margin: the margin is never text.
  (void)act(73 + 6, 4 + 23 - 6, M::press);
  (void)act(72 + 1, 4 + 22 - 6, M::drag);
  a = act(72 + 1, 4 + 22 - 6, M::release);
  check(a.copy == "  %0 = const.i32 0\n  %1 =", std::format("backwards from the margin: {:?}", a.copy.value_or("(nothing)")));

  // V in the pane: whole lines from the top one shown; j extends; y copies.
  (void)key("V");
  (void)key("j");
  (void)key("j");
  check(trace::strip_sgr(b.frame().lines.back()).contains("VISUAL") && trace::strip_sgr(b.frame().lines.back()).contains("j/k extend"), "V says what it does, in the footer");
  a = key("y");
  check(a.copy == "started  200µs\ntook     300µs\nworker   w2\n", std::format("V, j, j, y copies three lines: {:?}", a.copy.value_or("(nothing)")));
  (void)key("V");
  (void)key("\x1b");
  a = key("y");
  check(a.copy && a.copy->starts_with("FunctionIr\nsquare\n"), "Escape drops the selection: y copies the cell again");

  // V in the tree; y with nothing selected copies the cell; Y its subtree.
  (void)key("\x1b");  // the keys back to the tree
  (void)key("V");
  (void)key("j");
  a = key("y");
  check(a.copy == "* FunctionIr(square) executed 300µs w2 alloc 1000B, 3.9KiB in all, retains 300B\n"
                  "*   FunctionGen(square) executed 190µs w2 alloc 2.9KiB\n",
        std::format("V, j, y in the tree copies two rows: {:?}", a.copy.value_or("(nothing)")));
  (void)key("g");
  a = key("y");
  check(a.copy && a.copy->starts_with("Tu\na.cpp\n\n● executed\nran, value changed\n\nstarted  0ns\ntook     900µs\nworker   w0\n"),
        std::format("y copies the cell as the pane shows it: {:?}", a.copy.value_or("(nothing)")));
  // Memory as a table: the cell's own, and with everything below it.
  check(a.copy && a.copy->ends_with("memory\n            own   in all\nalloc    4.0KiB  29.1KiB\nallocs      128\npeak     4.0KiB\nretains  1.0KiB   6.9KiB\n\n"
                                    "no value shown for this kind\n"),
        std::format("its memory, and that it has no value: {:?}", a.copy.value_or("(nothing)")));
  (void)key("E");  // fold everything: Y copies what is folded too
  a = key("Y");
  const auto lines = a.copy ? std::ranges::count(*a.copy, '\n') : 0;
  check(lines == 10 && a.copy->starts_with("* Tu(a.cpp) executed 900µs w0 alloc 4.0KiB, 29.1KiB in all, retains 1.0KiB\n"),
        std::format("Y copies the whole subtree, folded or not ({} lines: 9 cells and an error line)", lines));
  check(trace::strip_sgr(b.frame().lines.back()).contains("copied the subtree (9 cells)"), "and says how much");

  // A selection is of cells, not row numbers, and goes when its rows or its pane change under it.
  const auto status = [&] {
    const trace::Frame f = b.frame();
    return trace::strip_sgr(f.lines.back());  // notes are in the footer
  };
  (void)key("e");
  (void)key("g");
  (void)key("V");
  (void)key("j");
  (void)key("j");
  (void)key("s");  // re-ordered: other cells now lie between its ends
  a = key("y");
  check(status().contains("copied the cell"), "a sort drops a selection: y copies the cell");
  (void)key("s"), (void)key("s"), (void)key("s");  // back to start order
  (void)act(10, 8, M::press);  // FunctionIr, whose pane is long
  (void)act(10, 8, M::release);
  (void)act(90, 7, M::press);
  (void)act(90, 7, M::release);
  (void)key("V");
  (void)key("j");
  (void)key("h");  // the keys go back to the tree: the pane's selection goes with them
  (void)key("j");
  a = key("y");
  check(status().contains("copied the cell") && a.copy && a.copy->starts_with("FunctionGen"), "leaving the pane drops its selection");
  (void)act(10, 8, M::press);
  (void)act(10, 8, M::release);
  (void)act(73 + 3, 7, M::press);
  (void)act(73 + 8, 7, M::drag);
  (void)key("<");  // the pane is wider now: its lines are others
  a = key("y");
  check(status().contains("copied the cell"), "a resized pane drops a selection");
  (void)key(">");
  // A click on the long pane's last row (where it says which lines it shows) scrolls nothing.
  (void)b.frame();
  const std::size_t scroll = b.detail_scroll();
  (void)act(90, 4 + 19, M::press);
  (void)act(90, 4 + 19, M::release);
  check(b.detail_scroll() == scroll, "a click on the pane's last row does not scroll it");
  // A drag over a blank line copies nothing, and says so.
  (void)act(73 + 2, 4 + 2 - b.detail_scroll(), M::press);
  (void)act(73 + 9, 4 + 2 - b.detail_scroll(), M::drag);
  a = act(73 + 9, 4 + 2 - b.detail_scroll(), M::release);
  check(!a.copy && status().contains("nothing to copy"), std::format("an empty selection copies nothing: {}", status()));

  // A live feed adding rows above keeps a selection on its cells.
  b.set_live("test");
  (void)act(10, 5, M::press);  // Pp (newest first: the one request is still on top)
  (void)act(10, 7, M::drag);
  (void)b.pump([&] {
    Event top = ev(100, 0, "Tu", "b.cpp", Outcome::executed, 950'000, 990'000);
    t.add(std::vector<Event>{top});
    return true;
  });
  a = act(10, 8, M::release);
  check(a.copy && a.copy->starts_with("* Pp(a.cpp)") && std::ranges::count(*a.copy, '\n') == 3,
        std::format("the rows stay the cells that were selected: {:?}", a.copy.value_or("(nothing)")));
  // A drag held past the bottom edge keeps scrolling on ticks; the wheel moves its end too.
  b.resize(10, 120);  // 6 tree rows of 11
  (void)b.frame();
  const std::size_t y0 = b.header_height() + 1;  // the first tree row
  (void)key("g");
  (void)act(10, y0, M::press);
  (void)act(10, y0 + 6, M::drag);  // the footer: past the edge
  check(b.ticking(), "a drag past the edge ticks");
  for (int i = 0; i < 8; ++i) b.tick();
  check(!b.ticking(), "at the end of the rows ticking stops (a release never reported stops there too)");
  a = act(10, y0 + 6, M::release);
  check(a.copy && std::ranges::count(*a.copy, '\n') >= 10, std::format("ticks scrolled the selection to the end: {}", status()));
  check(!b.ticking(), "and stop with the release");
  (void)key("g");
  (void)b.frame();  // the view at the top, as drawn
  (void)act(10, y0, M::press);
  (void)act(10, y0 + 1, M::drag);
  (void)act(10, y0 + 1, M::wheel_down);
  a = act(10, y0 + 1, M::release);
  check(status().contains("copied 4 rows"), std::format("the wheel during a drag moves its end: {}", status()));
  // V at the top of a live view: a request arriving keeps the selection's end where it was.
  b.resize(24, 120);
  (void)key("g");
  (void)key("V");
  const std::string top_cell = t.node(t.rows()[0].node).ev.key;
  (void)b.pump([&] {
    t.add(std::vector<Event>{ev(101, 0, "Tu", "c.cpp", Outcome::executed, 995'000, 999'000)});
    return true;
  });
  (void)key("j");
  a = key("y");
  check(a.copy && std::ranges::count(*a.copy, '\n') == 2 && a.copy->contains(top_cell) && !a.copy->contains("c.cpp"),
        std::format("V holds its cells while requests arrive above: {:?}", a.copy.value_or("(nothing)")));
}

// Source spans (#638): which cells say where they are in the source, read from values a traced
// build already holds; and the model's text, filter and JSON for them.
void spans() {
  using engine::SourceSpan;
  std::map<std::string, SourceSpan> got;  // `Kind(key)` -> span, the last event of each
  std::vector<Event> all;
  pl::Session s;
  trace::Collector c([&](std::span<const Event> batch) {
    for (const Event& e : batch) got[trace::cell_name(e)] = e.span, all.push_back(e);
  });
  s.set_source(tu, "spans.cpp", "int square(int x) {\n  return x * x;\n}\nint main() { return square(3) - 9; }\n", "spans.o");
  (void)s.get<pl::Tu>(pl::Tu::Key{tu}, &c);
  c.finish();
  const auto at = [&](std::string_view cell) { return got.contains(std::string(cell)) ? got[std::string(cell)] : SourceSpan{"(no event)"}; };
  check(at("FunctionIr(square)") == SourceSpan{"spans.cpp", 1, 1, 3, 1}, std::format("a function's IR is its definition: {}", trace::span_text(at("FunctionIr(square)"))));
  check(at("Lower(main)") == SourceSpan{"spans.cpp", 4, 1, 4, 36}, std::format("and its L2: {}", trace::span_text(at("Lower(main)"))));
  check(at("Codegen(square)") == SourceSpan{"spans.cpp", 1, 1, 3, 1}, "and its code");
  check(at("Tu(spans.cpp)") == SourceSpan{"spans.cpp"} && at("Pp(spans.cpp)") == SourceSpan{"spans.cpp"}, "a unit's cells are its file");
  for (const Event& e : all) {
    if (e.kind == "Optimized" || e.kind == "LayoutConstant" || e.kind == "CostModelIn" || e.kind == "OptLevel")
      check(e.span.file.empty(), std::format("{} has no span", trace::cell_name(e)));
  }
  check(std::ranges::count_if(all, [](const Event& e) { return !e.span.file.empty(); }) > 10, "most cells of a unit have spans");

  // An edit that moves the function: the second build's cells (cut off or verified) say where it
  // is now.
  got.clear();
  s.set_source(tu, "spans.cpp", "\n\nint square(int x) {\n  return x * x;\n}\nint main() { return square(3) - 9; }\n", "spans.o");
  (void)s.get<pl::Tu>(pl::Tu::Key{tu}, &c);
  c.finish();
  check(at("FunctionIr(square)") == SourceSpan{"spans.cpp", 3, 1, 5, 1}, std::format("after an edit, where it is now: {}", trace::span_text(at("FunctionIr(square)"))));

  // A function a header defines, read from disk; and in the editor view, from the open buffer.
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / std::format("ycxx-spans-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  fs::create_directories(dir);
  const std::string header = "// cube\ninline int cube(int x) { return x * x * x; }\n";
  const std::string main_text = "#include \"h.h\"\nint main() { return cube(1) - 1; }\n";
  std::ofstream(dir / "h.h", std::ios::binary) << header;
  std::ofstream(dir / "m.cpp", std::ios::binary) << main_text;
  pl::Session hs;
  constexpr pl::TuId htu{1};
  got.clear();
  hs.set_source(htu, "m.cpp", main_text, "m.o", dir.native());
  (void)hs.get<pl::Tu>(pl::Tu::Key{htu}, &c);
  c.finish();
  const SourceSpan cube = at("FunctionIr(cube)");
  check(cube.file.ends_with("h.h") && cube.line == 2 && cube.column == 1 && cube.end_line == 2 && cube.end_column == 44,
        std::format("a header's function, in the header: {}", trace::span_text(cube)));
  const pl::TuId editor{1u << 31};
  ycxx::pp::Config editor_config;
  editor_config.view = ycxx::pp::View::editor;
  hs.db().set<ycxx::pp::Buffer>(ycxx::pp::path_id((dir / "h.h").lexically_normal().generic_string<char>()), "// a\n// b\n" + header);
  got.clear();
  hs.set_source(editor, "m.cpp", "", "m.o", dir.native(), editor_config);
  (void)hs.get<pl::Tu>(pl::Tu::Key{editor}, &c);
  c.finish();
  const SourceSpan open = at("FunctionIr(cube)");
  check(open.file.ends_with("h.h") && open.line == 4, std::format("the editor view counts the buffer's lines: {}", trace::span_text(open)));
  fs::remove_all(dir);

  check(trace::span_start(SourceSpan{"a.cpp", 3, 5, 7, 1}) == "a.cpp:3:5" && trace::span_text(SourceSpan{"a.cpp", 3, 5, 7, 1}) == "a.cpp:3:5–7:1" &&
            trace::span_text(SourceSpan{"a.cpp", 3, 5, 3, 12}) == "a.cpp:3:5–12" && trace::span_text(SourceSpan{"a.cpp"}) == "a.cpp" &&
            trace::span_text(SourceSpan{}).empty(),
        "span texts");
  const SourceSpan f{"/src/dir/foo.cpp", 120, 1, 140, 1};
  check(trace::span_touches(f, "foo.cpp:120") && trace::span_touches(f, "dir/foo.cpp:130") && trace::span_touches(f, "/src/dir/foo.cpp") &&
            !trace::span_touches(f, "foo.cpp:141") && !trace::span_touches(f, "oo.cpp:120") && !trace::span_touches(f, "bar.cpp"),
        "a span touches a line of its file, by a path's tail");
  check(trace::span_touches(SourceSpan{"foo.cpp"}, "foo.cpp:9") && !trace::span_touches(SourceSpan{}, "foo.cpp"), "a whole file touches every line");
  check(trace::span_touches(f, "foo.cpp:120:7") && !trace::span_touches(f, "foo.cpp:141:1"), "file:line:column, as span_start writes it");

  trace::Tree t;
  t.add(all);
  t.set_filter("at:spans.cpp:2");
  std::set<std::string> kinds;
  for (const auto& n : t.nodes())
    if (t.matches(n.ev)) kinds.insert(trace::cell_name(n.ev));
  check(kinds.contains("FunctionIr(square)") && !kinds.contains("FunctionIr(main)") && kinds.contains("Tu(spans.cpp)"),
        "at:spans.cpp:2 keeps square's cells and the unit's, not main's");
  t.set_filter("at:Spans.cpp");
  check(t.rows().empty(), "paths keep their case");
  Event e = ev(1, 0, "Lower", "square", Outcome::executed);
  e.span = SourceSpan{"a.cpp", 1, 2, 3, 4};
  check(trace::json_line(e).ends_with(R"(,"span":{"file":"a.cpp","line":1,"column":2,"end_line":3,"end_column":4}})"), "the JSON line's span");
  check(!trace::json_line(ev(1, 0, "Link", "0", Outcome::executed)).contains("span"), "none without one");
}

// Histograms (#637): what groups of cells cost, the browser's view of them, and the JSON's.
void histograms(const std::filesystem::path& golden_path, bool update) {
  using trace::Metric;
  using trace::Grouping;
  check(trace::uncovered(0, 100, {{10, 30}, {20, 50}, {90, 120}}) == 50 && trace::uncovered(0, 100, {}) == 100 && trace::uncovered(5, 5, {{0, 9}}) == 0,
        "self time: the span less the union of its children's, clipped");
  std::vector<Event> events = browser_events();
  for (Event& e : events) {
    if (e.kind == "Pp" || (e.kind == "FileText" && e.key == "a.cpp") || e.kind == "FunctionIr" || e.kind == "FunctionGen")
      e.span = engine::SourceSpan{"a.cpp", 1, 1, 3, 1};
    if (e.key == "x.h") e.span = engine::SourceSpan{"x.h"};
  }
  const std::vector<std::uint64_t> self = trace::self_times(events);
  check(self == std::vector<std::uint64_t>{60'000, 190'000, 0, 0, 110'000, 190'000, 110'000, 240'000, 0},
        std::format("self times: {}", self));
  trace::Tree t;
  t.add(events);
  const auto names = [](const std::vector<trace::Group>& gs) {
    std::vector<std::string> out;
    for (const trace::Group& g : gs) out.push_back(std::format("{}:{}", g.name, g.cells));
    return out;
  };
  const auto by_kind = trace::histogram(t, Grouping::kind, std::nullopt, Metric::self_time);
  check(names(by_kind) == std::vector<std::string>{"Optimized:1", "FunctionGen:1", "Pp:1", "Codegen:1", "FunctionIr:1", "Tu:1", "FileText:2", "SourceOf:3"},
        std::format("by kind, most self time first, ties by name: {}", names(by_kind)));
  const trace::Group& tu = by_kind[5];
  check(tu.at(Metric::self_time) == 60'000 && tu.at(Metric::total_time) == 900'000 && tu.at(Metric::alloc) == 4096 && tu.at(Metric::retained) == 1024 &&
            tu.at(Metric::executions) == 1 && tu.at(Metric::hits) == 0,
        "a group's metrics");
  check(by_kind[7].at(Metric::hits) == 3 && by_kind[6].at(Metric::retained) == 500, "cache hits, each read of a folded cell counted; sums over cells");
  check(names(trace::histogram(t, Grouping::kind, std::nullopt, Metric::retained)).front() == "Pp:1", "sorted by another metric");
  check(names(trace::histogram(t, Grouping::file, std::nullopt, Metric::total_time)) == std::vector<std::string>{"(no source):6", "a.cpp:4", "x.h:1"},
        std::format("by file: {}", names(trace::histogram(t, Grouping::file, std::nullopt, Metric::total_time))));
  const auto children = trace::histogram(t, Grouping::children, 0, Metric::total_time);
  check(names(children) == std::vector<std::string>{"Codegen(main):2", "FunctionIr(square):2", "Pp(a.cpp):3", "SourceOf(a.cpp):3"} &&
            children[0].node == 6 && children[0].at(Metric::total_time) == 350'000,
        std::format("by children of Tu, over their subtrees: {}", names(children)));
  check(names(trace::histogram(t, Grouping::kind, 4, Metric::self_time)) == std::vector<std::string>{"FunctionGen:1", "FunctionIr:1"},
        "within a cell's subtree");
  t.set_filter("is:input");
  check(names(trace::histogram(t, Grouping::kind, std::nullopt, Metric::self_time)) == std::vector<std::string>{"FileText:2"}, "the tree's filter applies");
  t.set_filter("kind:FileText");
  check(t.rows().size() == 4 && t.matches(events[2]) && !t.matches(events[0]), "kind: matches a whole kind");
  t.set_filter("kind:File");
  check(!t.matches(events[2]), "and only a whole one");
  t.set_filter("");

  // JSON: the stream's summary carries the same groups.
  std::ostringstream js;
  trace::Stream st(js, {false, true, true});
  st.batch(events);
  st.summary();
  const std::string json = js.str();
  check(json.contains(R"("histogram":{"by_kind":[{"kind":"Optimized","cells":1,"self_ns":240000,"total_ns":240000,"alloc_bytes":700,"peak_bytes":700,"retained_bytes":64,"allocs":21,"executions":1,"hits":0,"waits":0},{"kind":"FunctionGen")") &&
            json.contains(R"x("by_file":[{"file":"a.cpp","cells":4,"self_ns":490000,"total_ns":490000)x") && json.contains(R"x({"file":"(no source)","cells":6,"self_ns":410000,"total_ns":900000)x"),
        std::format("the JSON summary's histogram: {}", json.substr(json.find("\"by_file\""), 200)));

  // The view: b opens it; its switches, grouping and headers click; a bar shows its cells.
  t.expand_all(false);
  trace::Browser b(t);
  b.resize(24, 120);
  std::string got;
  const auto step = [&](std::string_view name) {
    got += std::format("## {}\n", name);
    got += plain(b.frame());
  };
  std::chrono::milliseconds clock{0};
  const auto press = [&](std::size_t x, std::size_t y) {
    (void)b.handle(click(x, y), clock += std::chrono::seconds(1));
    return b.handle(click(x, y, trace::Mouse::Act::release), clock);
  };
  using H = trace::Hit;
  (void)b.handle(trace::Key{"b"});
  check(b.view() == trace::View::histogram, "b opens the histogram");
  step("the histogram: by kind, most self time first");
  // Body rows from 2: the switches, the grouping, the headers, the bars.
  check(b.hit(10, 3) == H{H::Area::metric, 0} && b.hit(24, 3) == H{H::Area::metric, 1}, std::format("the switches ({})", std::to_underlying(b.hit(24, 3).area)));
  check(b.hit(11, 4) == H{H::Area::grouping, 0} && b.hit(17, 4) == H{H::Area::grouping, 1} && b.hit(23, 4) == H{H::Area::grouping, 2},
        "the groupings");
  check(b.hit(37, 4) == H{H::Area::scope}, "the scope");
  check(b.hit(30, 5) == H{H::Area::order, 0} && b.hit(5, 6) == H{H::Area::bar, 0} && b.hit(5, 13) == H{H::Area::bar, 7} && b.hit(5, 14) == H{},
        "the headers and the bars");
  check(b.hit(30, 0) == H{H::Area::tab, std::to_underlying(trace::View::histogram)}, "its tab");
  press(41, 3);  // □4 peak, the fourth switch (x 40–46)
  const auto shown = [&](std::string_view m) { return trace::strip_sgr(b.frame().lines[5]).contains(m); };
  check(shown("peak"), "a click on a switch shows its metric");
  (void)b.handle(trace::Key{"4"});
  check(!shown("peak"), "and its key hides it");
  for (const char* k : {"1", "2", "3", "7"}) (void)b.handle(trace::Key{k});
  check(shown("runs") && !shown("self time"), "the last metric stays (runs)");
  for (const char* k : {"2", "3", "7"}) (void)b.handle(trace::Key{k});
  (void)b.handle(trace::Key{"s"});
  // Shown: time in all and allocated; the sort moved off each metric switched off, to time in all.
  check(trace::strip_sgr(b.frame().lines[5]).contains("allocated↓"), "s sorts by the next metric");
  press(17, 4);
  step("by file, by time in all");
  press(5, 7);  // the second bar: (no source)
  check(b.view() == trace::View::histogram, "a cell with no source opens nothing");
  press(5, 6);  // a.cpp, the most allocated
  check(b.view() == trace::View::tree && t.filtering() && t.rows().size() >= 4, "a click on a bar shows its cells in the tree");
  check(trace::strip_sgr(b.frame().lines[2]).contains("filter: at:=a.cpp"), "as a filter, exactly that file");
  (void)b.handle(trace::Key{"/"});
  (void)b.handle(trace::Key{"\x7f"});
  for (int i = 0; i < 12; ++i) (void)b.handle(trace::Key{"\x7f"});
  (void)b.handle(trace::Key{"\r"});
  (void)b.handle(trace::Key{"b"});
  press(11, 4);  // by kind
  (void)b.handle(trace::Key{"\r"});  // Optimized
  check(b.view() == trace::View::tree && t.rows().size() == 2 && t.node(t.rows()[1].node).ev.kind == "Pp",
        "Enter opens the selected bar (Pp: the most allocated, under Tu)");
  (void)b.handle(trace::Key{"/"});
  for (int i = 0; i < 20; ++i) (void)b.handle(trace::Key{"\x7f"});
  (void)b.handle(trace::Key{"\r"});
  (void)b.handle(trace::Key{"g"});  // Tu
  (void)b.handle(trace::Key{"b"});
  (void)b.handle(trace::Key{"c"});
  (void)b.handle(trace::Key{"c"});
  step("by the children of the selected cell");
  (void)b.handle(trace::Key{"j"});
  (void)b.handle(trace::Key{"\r"});
  check(b.view() == trace::View::tree && t.node(t.rows()[b.cursor()].node).ev.kind == "FunctionIr", "a child's bar selects it in the tree");
  (void)b.handle(click(5, 7, trace::Mouse::Act::wheel_down));

  // Spans in the browser (#638): the pane's `source` line, the S column, p to copy.
  t.expand_all(true);
  (void)b.handle(trace::Key{"g"});
  for (int i = 0; i < 4; ++i) (void)b.handle(trace::Key{"j"});  // FunctionIr(square)
  const auto frame_text = [&] {
    std::string out;
    for (const std::string& l : b.frame().lines) out += trace::strip_sgr(l) + '\n';
    return out;
  };
  check(frame_text().contains("source   a.cpp:1:1–3:1"), "the pane says where the cell is");
  (void)b.handle(trace::Key{"S"});
  step("the source column (S)");
  check(frame_text().contains("source") && frame_text().contains("a.cpp:1"), "S shows the source column");
  trace::Action a = b.handle(trace::Key{"p"});
  check(a.copy == "a.cpp:1:1", std::format("p copies path:line:col: {:?}", a.copy.value_or("(nothing)")));
  (void)b.handle(trace::Key{"g"});
  a = b.handle(trace::Key{"p"});
  check(!a.copy && trace::strip_sgr(b.frame().lines.back()).contains("no source for this cell"), "a cell without a span copies nothing");
  (void)b.handle(trace::Key{"/"});
  for (char c : std::string_view("at:a.cpp:2")) (void)b.handle(trace::Key{std::string(1, c)});
  (void)b.handle(trace::Key{"\r"});
  check(t.matches(events[4]) && !t.matches(events[6]), "at: in the browser's filter");

  // The JSON and the view agree: the stream's groups are the tree's, for the same events.
  {
    trace::Tree all;
    all.add(events);
    const std::string from_tree = trace::groups_json(trace::histogram(all, Grouping::kind, std::nullopt, Metric::self_time), "kind");
    check(json.contains(R"("by_kind":)" + from_tree), "the JSON's groups by kind are the view's");
    const std::string files = trace::groups_json(trace::histogram(all, Grouping::file, std::nullopt, Metric::self_time), "file");
    check(json.contains(R"("by_file":)" + files), "and by file");
  }
  // A child after unrelated events in a batch (the collector's `finish`) counts under its parent.
  {
    std::vector<Event> late = {ev(1, 0, "A", "a", Outcome::executed, 0, 100), ev(2, 0, "B", "b", Outcome::executed, 0, 50),
                               ev(3, 1, "C", "c", Outcome::executed, 10, 40)};
    late[2].depth = 1;
    check(trace::self_times(late) == std::vector<std::uint64_t>{70, 50, 30}, std::format("self times by parent id: {}", trace::self_times(late)));
  }
  // A group nested in itself counts its time in all once.
  {
    std::vector<Event> nested = {ev(1, 0, "I", "x", Outcome::executed, 0, 100), ev(2, 1, "I", "y", Outcome::executed, 10, 90),
                                 ev(3, 2, "I", "z", Outcome::executed, 20, 80)};
    nested[1].depth = 1, nested[2].depth = 2;
    trace::Tree nt;
    nt.add(nested);
    const auto g = trace::histogram(nt, Grouping::kind, std::nullopt, Metric::total_time);
    check(g.size() == 1 && g[0].at(Metric::total_time) == 100 && g[0].at(Metric::self_time) == 100, "nested in itself: once, in all and by self");
    Event w = ev(9, 0, "W", "w", Outcome::waited, 0, 500);
    check(trace::metrics_of(w, 500)[std::to_underlying(Metric::self_time)] == 0 && trace::metrics_of(w, 500)[std::to_underlying(Metric::total_time)] == 0 &&
              trace::metrics_of(w, 500)[std::to_underlying(Metric::waits)] == 1,
          "a wait counts as a wait, not as time spent");
  }
  // Children of one name are separate bars.
  {
    std::vector<Event> twins = {ev(1, 0, "P", "p", Outcome::executed, 0, 100), ev(2, 1, "L", "f", Outcome::executed, 0, 10),
                                ev(3, 1, "L", "f", Outcome::reused, 20, 20)};
    twins[1].depth = twins[2].depth = 1;
    trace::Tree tt;
    tt.add(twins);
    const auto g = trace::histogram(tt, Grouping::children, 0, Metric::total_time);
    check(g.size() == 2 && g[0].node != g[1].node, "two children named alike are two bars");
  }

  // The view, after the review: Tab keeps to the tree and timeline, 0 does nothing here, the wheel
  // moves the selected bar, a bar adds to the filter, the second press of a double click on a
  // bar is no click, and paths with spaces quote.
  b.resize(24, 120);
  (void)b.handle(trace::Key{"/"});
  for (int i = 0; i < 30; ++i) (void)b.handle(trace::Key{"\x7f"});
  for (char c : std::string_view("is:ex")) (void)b.handle(trace::Key{std::string(1, c)});
  (void)b.handle(trace::Key{"\r"});
  (void)b.handle(trace::Key{"\t"});
  check(b.view() == trace::View::timeline, "Tab goes to the timeline");
  (void)b.handle(trace::Key{"\t"});
  check(b.view() == trace::View::tree, "and back to the tree, as ever");
  (void)b.handle(trace::Key{"b"});
  (void)b.handle(trace::Key{"0"});
  check(!t.worker(), "0 picks no worker in the histogram");
  (void)b.handle(trace::Key{"c"});  // by kind again
  const std::size_t y_bars = 6;
  (void)b.handle(click(5, y_bars, trace::Mouse::Act::wheel_down));
  (void)b.frame();
  (void)b.handle(click(5, y_bars, trace::Mouse::Act::wheel_up));
  clock += std::chrono::seconds(5);
  (void)b.handle(click(5, y_bars), clock);
  check(b.view() == trace::View::tree && trace::strip_sgr(b.frame().lines[2]).contains("filter: is:ex kind:"),
        std::format("a bar adds its kind to the filter: {}", trace::strip_sgr(b.frame().lines[2])));
  const std::size_t at_open = b.cursor();
  (void)b.handle(click(5, 9), clock + std::chrono::milliseconds(100));
  check(b.cursor() == at_open, "the second press of a double click on a bar moves nothing");
  // A bar keeps the user's own at: when it adds a kind; quoted words survive the round trip.
  check(trace::filter_text(trace::filter_words(R"(is:ex at:"=/my dir/a.cpp" kind:Pp)")) == R"(is:ex at:"=/my dir/a.cpp" kind:Pp)",
        "filter words round-trip, quotes and all");
  (void)b.handle(trace::Key{"/"});
  for (int i = 0; i < 40; ++i) (void)b.handle(trace::Key{"\x7f"});
  for (char c : std::string_view("at:a.cpp")) (void)b.handle(trace::Key{std::string(1, c)});
  (void)b.handle(trace::Key{"\r"});
  (void)b.handle(trace::Key{"b"});
  (void)b.handle(trace::Key{"\r"});
  check(trace::strip_sgr(b.frame().lines[2]).contains("filter: at:a.cpp kind:"), std::format("the user's at: stays: {}", trace::strip_sgr(b.frame().lines[2])));
  // Parent ids that form a cycle (from the wire) end the walk.
  {
    std::vector<Event> cyc = {ev(1, 2, "A", "a", Outcome::executed, 0, 10), ev(2, 1, "B", "b", Outcome::executed, 0, 10)};
    std::ostringstream sink;
    trace::Stream cs(sink, {false, true, true});
    cs.batch(cyc);
    cs.summary();
    check(sink.str().contains("\"by_kind\""), "a cycle of parents ends");
  }
  t.set_filter(R"(at:"=/my dir/a.cpp")");
  Event spaced = ev(50, 0, "Pp", "q", Outcome::executed);
  spaced.span = engine::SourceSpan{"/my dir/a.cpp"};
  check(t.matches(spaced) && !t.matches(events[0]), "a quoted path with a space matches exactly");
  t.set_filter("");

  if (update) {
    std::ofstream(golden_path, std::ios::binary) << got;
    std::println("updated {}", golden_path.native());
    return;
  }
  std::ifstream in(golden_path, std::ios::binary);
  const std::string want{std::istreambuf_iterator<char>(in), {}};
  check(got == want, "the histogram's frames equal histogram.golden (rerun with --update after a deliberate change)");
  if (got != want) std::println(std::cerr, "--- got:\n{}", got);
}

// ---- A live trace (#939): the tree built as events come, in any order, against the one the
// collector's batches build.

// A simulated request: frames, which report their start and their end, and instants under them,
// some of them reads of one cell in a row (which fold), and some frames that take no time. Ids
// are given as each starts; times grow with them.
struct Sim {
  struct Item {
    std::uint64_t id = 0, parent = 0;
    bool frame = false;
    std::vector<std::size_t> kids;  // items
    engine::TraceEvent end;
  };
  std::vector<Item> items;  // [0] the request's own frame
  std::chrono::steady_clock::time_point origin;

  static constexpr std::array<std::string_view, 5> kinds{"ycxx::pipeline::Tu", "Pp", "FileText", "Lower", "Codegen"};

  Sim(std::mt19937& rng, std::chrono::steady_clock::time_point at, std::uint64_t first_id = 1) : origin(at) {
    next_ = first_id;
    make(rng, 0, 0, 0);
    finish_times(0);
  }

  // Every engine event, starts (`started`) and ends, in an order the engine could report them in:
  // a frame's start before anything under it, everything under it ended before it ends.
  std::vector<std::pair<bool, engine::TraceEvent>> valid_order(std::mt19937& rng) const {
    std::vector<std::pair<bool, engine::TraceEvent>> out;
    std::vector<std::pair<std::size_t, bool>> ready{{0, true}};  // (item, its start)
    std::vector<std::size_t> left(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) left[i] = items[i].kids.size();
    while (!ready.empty()) {
      const std::size_t k = std::uniform_int_distribution<std::size_t>(0, ready.size() - 1)(rng);
      const auto [i, start] = ready[k];
      ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(k));
      const Item& it = items[i];
      if (start) {
        engine::TraceEvent s = it.end;
        s.end = s.start;
        s.alloc_bytes = s.allocs = 0;
        s.peak_bytes = s.retained_bytes = 0;
        s.errors.clear();
        out.emplace_back(true, std::move(s));
        for (std::size_t c : it.kids) ready.emplace_back(c, items[c].frame);
        if (it.kids.empty()) ready.emplace_back(i, false);
        continue;
      }
      out.emplace_back(false, it.end);
      if (i == 0) continue;
      const std::size_t p = parent_of_[i];
      if (--left[p] == 0) ready.emplace_back(p, false);
    }
    return out;
  }

 private:
  void make(std::mt19937& rng, std::size_t at, std::uint64_t parent, int depth) {
    if (items.size() <= at) items.resize(at + 1), parent_of_.resize(at + 1);
    Item& me = items[at];
    me.id = next_++;
    me.parent = parent;
    me.frame = true;
    engine::TraceEvent& e = me.end;
    e.id = me.id;
    e.parent = parent;
    e.kind = kinds[std::uniform_int_distribution<std::size_t>(0, kinds.size() - 1)(rng)];
    e.key = std::format("f{}", me.id);
    e.cell = 1000 + me.id;
    e.worker = std::uniform_int_distribution<std::int32_t>(-1, 3)(rng);
    e.end_worker = std::uniform_int_distribution<int>(0, 3)(rng) == 0 ? std::uniform_int_distribution<std::int32_t>(-1, 3)(rng) : e.worker;
    const int pick = std::uniform_int_distribution<int>(0, 9)(rng);
    e.outcome = pick < 6 ? Outcome::executed : pick < 8 ? Outcome::cutoff : Outcome::verified;
    e.start = origin + std::chrono::nanoseconds(me.id * 100);
    e.alloc_bytes = std::uniform_int_distribution<std::uint64_t>(0, 4096)(rng);
    e.allocs = e.alloc_bytes / 32;
    e.peak_bytes = static_cast<std::int64_t>(e.alloc_bytes);
    e.retained_bytes = std::uniform_int_distribution<std::int64_t>(-64, 1024)(rng);
    e.span.file = std::uniform_int_distribution<int>(0, 2)(rng) == 0 ? "" : std::format("f{}.cpp", me.id % 3);
    if (std::uniform_int_distribution<int>(0, 19)(rng) == 0) e.errors = "a.cpp:1:1: error: no";
    const int kids = depth >= 4 ? 0 : std::uniform_int_distribution<int>(0, depth == 0 ? 8 : 5)(rng);
    std::optional<engine::TraceEvent> last_instant;
    for (int k = 0; k < kids; ++k) {
      const std::size_t c = items.size();
      items.resize(c + 1);
      parent_of_.resize(c + 1);
      parent_of_[c] = at;
      items[at].kids.push_back(c);
      if (std::uniform_int_distribution<int>(0, 9)(rng) < 4 && depth < 4) {
        make(rng, c, items[at].id, depth + 1);
        last_instant.reset();
        continue;
      }
      // An instant: the same cell read again, often, so that reads in a row fold.
      Item& in = items[c];
      in.id = next_++;
      in.parent = items[at].id;
      engine::TraceEvent i;
      if (last_instant && std::uniform_int_distribution<int>(0, 2)(rng) > 0) {
        i = *last_instant;
      } else {
        i.kind = kinds[std::uniform_int_distribution<std::size_t>(0, kinds.size() - 1)(rng)];
        i.cell = std::uniform_int_distribution<std::uint64_t>(1, 6)(rng);
        i.key = std::format("c{}", i.cell);
        const int o = std::uniform_int_distribution<int>(0, 3)(rng);
        i.outcome = o == 0 ? Outcome::reused : o == 1 ? Outcome::input : o == 2 ? Outcome::durable : Outcome::waited;
        i.retained_bytes = i.outcome == Outcome::input ? 100 : 0;
        i.span.file = "a.cpp";
      }
      i.id = in.id;
      i.parent = in.parent;
      i.start = i.end = origin + std::chrono::nanoseconds(in.id * 100);
      i.worker = i.end_worker = items[at].end.worker;
      in.end = i;
      last_instant = i;
    }
  }
  // A frame ends after what is under it; a frame with nothing under it may take no time at all.
  std::chrono::steady_clock::time_point finish_times(std::size_t at) {
    Item& me = items[at];
    if (!me.frame) return me.end.end;
    auto end = me.end.start;
    for (std::size_t c : me.kids) end = std::max(end, finish_times(c));
    const bool none = me.kids.empty() && me.id % 4 == 0;
    items[at].end.end = none ? end : end + std::chrono::nanoseconds(50);
    return items[at].end.end;
  }

  std::uint64_t next_ = 1;
  std::vector<std::size_t> parent_of_;
};

// Whatever order events came in, a tree's sums, depths, order, folding and summary agree with its
// nodes: what each step of a live trace must keep. Empty: they do.
std::string inconsistent(const trace::Tree& t) {
  const auto nodes = t.nodes();
  const auto at = [&](std::int32_t i) -> const trace::Tree::Node& { return nodes[static_cast<std::size_t>(i)]; };
  const auto instant_leaf = [&](std::int32_t i) {
    return !at(i).ev.running && at(i).ev.start_ns == at(i).ev.end_ns && at(i).children.empty();
  };
  trace::Summary want;
  std::int32_t top = -1;
  for (std::size_t k = 0; k < nodes.size(); ++k) {
    const auto i = static_cast<std::int32_t>(k);
    const trace::Tree::Node& n = at(i);
    if (n.folded_into >= 0) {
      const auto& head = at(n.folded_into).members;
      if (std::ranges::find(head, i) == head.end()) return std::format("node {} is folded into {}, which does not list it", n.ev.id, at(n.folded_into).ev.id);
      continue;
    }
    std::uint64_t subtree = 1, exec = trace::ran(n.ev) && !n.group ? 1 : 0, alloc = n.group ? 0 : n.ev.alloc_bytes;
    std::int64_t retained = n.group ? 0 : trace::counted_retained(n.ev);
    std::uint32_t repeat = n.own_repeat;
    for (std::int32_t m : n.members) repeat += at(m).own_repeat;
    if (!n.group && repeat != n.ev.repeat) return std::format("node {}: repeat {}, its members say {}", n.ev.id, n.ev.repeat, repeat);
    std::uint64_t prev_id = 0;
    for (std::size_t c = 0; c < n.children.size(); ++c) {
      const trace::Tree::Node& k2 = at(n.children[c]);
      if (k2.parent != i) return std::format("node {}'s child {} names another parent", n.ev.id, k2.ev.id);
      if (k2.folded_into >= 0) return std::format("node {} shows folded {}", n.ev.id, k2.ev.id);
      if (c > 0 && k2.ev.id <= prev_id) return std::format("node {}'s children out of id order", n.ev.id);
      prev_id = k2.ev.id;
      if (n.attached && k2.ev.depth != (n.group ? 0 : n.ev.depth + 1)) return std::format("node {} at depth {} under {}", k2.ev.id, k2.ev.depth, n.ev.id);
      if (!n.group && n.attached && c > 0 && instant_leaf(n.children[c - 1]) && instant_leaf(n.children[c])) {
        const Event &a = at(n.children[c - 1]).ev, &b = k2.ev;
        if (a.cell == b.cell && a.kind == b.kind && a.outcome == b.outcome && a.errors == b.errors)
          return std::format("node {}: alike instants {} and {} not folded", n.ev.id, a.id, b.id);
      }
      subtree += k2.subtree;
      exec += k2.executions;
      alloc += k2.incl.alloc;
      retained += k2.incl.retained;
    }
    if (n.subtree != subtree || n.executions != exec || n.incl.alloc != alloc || n.incl.retained != retained)
      return std::format("node {}: sums {} {} {} {}, its children's {} {} {} {}", n.ev.id, n.subtree, n.executions, n.incl.alloc, n.incl.retained,
                         subtree, exec, alloc, retained);
    if (n.group) continue;
    if (n.ev.running) ++want.running;
    else ++want.counts[std::to_underlying(n.ev.outcome)], ++want.events, want.add_memory(n.ev);
    want.span_ns = std::max(want.span_ns, n.ev.end_ns);
    top = std::max({top, n.ev.worker, n.ev.end_worker});
  }
  want.workers = top + 1;
  const trace::Summary s = t.summary();
  if (s.counts != want.counts || s.events != want.events || s.running != want.running || s.span_ns != want.span_ns || s.workers != want.workers ||
      s.alloc_bytes != want.alloc_bytes || s.allocs != want.allocs || s.retained_bytes != want.retained_bytes)
    return std::format("the summary ({} events, {} running, span {}, {} workers) is not its nodes' ({}, {}, {}, {})", s.events, s.running, s.span_ns,
                       s.workers, want.events, want.running, want.span_ns, want.workers);
  for (const auto& r : t.rows())
    if (!at(r.node).attached || at(r.node).folded_into >= 0) return std::format("row of node {}, which is not shown", at(r.node).ev.id);
  return {};
}

// Everything a tree shows, by event id (node indices differ with arrival order): each node in
// pre-order, the summary, the rows, the timeline, the histograms, and the browser's frames.
std::string shown(const trace::Tree& given) {
  trace::Tree t = given;
  std::string out;
  const auto id = [&](std::int32_t i) { return i < 0 ? 0 : t.node(i).ev.id; };
  for (std::int32_t i : t.preorder()) {
    const trace::Tree::Node& n = t.node(i);
    const Event& e = n.ev;
    out += std::format("{} {} p{} {} {} {} {} {}-{} w{}→{} ×{} sub{} ran{} {}/{} {} {} {} {}\n", std::string(e.depth, ' '), e.id, id(n.parent), e.kind, e.key,
                       trace::state_name(e), e.cell, e.start_ns, e.end_ns, e.worker, e.end_worker, e.repeat, n.subtree, n.executions, n.incl.alloc,
                       n.incl.retained, n.expanded, e.errors, e.span.file, e.alloc_bytes);
  }
  const trace::Summary s = t.summary();
  out += std::format("summary {} {} {} {} {} {} {} {}\n", s.counts, s.events, s.running, s.span_ns, s.workers, s.alloc_bytes, s.allocs, s.retained_bytes);
  for (const auto& r : t.rows()) out += std::format("row {} {} {}\n", id(r.node), r.depth, r.match);
  for (const auto& lane : trace::timeline(t, 37).lanes) {
    out += "lane";
    for (std::int32_t n : lane) out += std::format(" {}", id(n));
    out += '\n';
  }
  for (auto by : {trace::Grouping::kind, trace::Grouping::file, trace::Grouping::children}) {
    for (const trace::Group& g : trace::histogram(t, by, std::nullopt, trace::Metric::self_time))
      out += std::format("group {} {} {} {}\n", g.name, g.cells, g.v, id(g.node));
  }
  trace::Browser b(t);
  b.resize(30, 120);
  out += plain(b.frame());
  for (const char* k : {"e", "t", "b", "c", "b", "j", "j", "\r"}) {
    (void)b.handle(trace::Key{k});
    out += plain(b.frame());
  }
  return out;
}

void live() {
  // A start, then its end; an instant; a running node's elapsed time; the summary while it runs.
  {
    trace::Tree t;
    Event s = ev(1, 0, "Build", "a.cpp", Outcome::executed, 100, 100, 2);
    s.running = true;
    t.apply(std::span(&s, 1));
    check(t.nodes().size() == 1 && t.node(0).ev.running && t.running() == 1 && t.summary().events == 0, "a start makes a running node, not yet an event");
    t.advance(5'000);
    check(t.node(0).ev.end_ns == 5'000 && t.summary().span_ns == 5'000, "a running node's time reaches now");
    Event c = ev(3, 2, "Pp", "a.cpp", Outcome::executed, 200, 900, 1);
    t.apply(std::span(&c, 1));
    check(t.rows().size() == 1 && !t.node(1).attached && t.summary().events == 1, "an end whose parent has not come waits, unseen, but counts");
    Event p = ev(2, 1, "Tu", "a.cpp", Outcome::executed, 150, 1000, 0);
    t.apply(std::span(&p, 1));
    check(t.rows().size() == 3 && t.node(1).attached && t.node(1).ev.depth == 2 && t.node(0).subtree == 3, "its parent comes: it joins under it");
    Event end = ev(1, 0, "Build", "a.cpp", Outcome::executed, 100, 2'000, 2);
    end.alloc_bytes = 64;
    t.apply(std::span(&end, 1));
    check(!t.node(0).ev.running && t.node(0).ev.end_ns == 2'000 && t.running() == 0 && t.node(0).incl.alloc == 64 && t.node(0).executions == 3,
          "its end settles it: its outcome, end and memory");
    check(inconsistent(t).empty(), inconsistent(t));
  }
  // A node that ended as an instant, folded into the one before it, gets a child (its end came
  // first): it is no instant, and what was folded around it folds again as the collector folds
  // it. In either order the same tree.
  for (bool backwards : {false, true}) {
    trace::Tree t;
    const auto r = [](std::uint64_t id, std::uint64_t parent, std::uint64_t at, std::uint64_t cell) {
      Event e = ev(id, parent, "K", std::format("k{}", cell), Outcome::reused, at, at, 0);
      e.cell = cell;
      return e;
    };
    Event top = ev(1, 0, "Build", "a", Outcome::executed, 0, 0, 0);
    top.running = true;
    std::vector<Event> es{top, r(2, 1, 10, 7), r(3, 1, 11, 7), r(4, 1, 12, 7), r(6, 1, 13, 7), r(5, 3, 11, 8)};
    if (backwards) std::ranges::reverse(es);
    t.apply(es);
    check(inconsistent(t).empty(), inconsistent(t));
    check(t.roots().size() == 1, "one top");
    const auto kids = t.node(t.roots()[0]).children;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> got;
    for (std::int32_t c : kids) got.emplace_back(t.node(c).ev.id, t.node(c).ev.repeat);
    using P = std::pair<std::uint64_t, std::uint32_t>;
    check(got == std::vector<P>{{2, 1}, {3, 1}, {4, 2}}, std::format("an instant with a child is no instant: 2, 3 (with 5 under it), 4 ×2 ({}): {}", backwards ? "backwards" : "in order", got));
  }
  // A request that ends with cells still running (an abandoned run): they failed.
  {
    trace::Tree t;
    const std::uint64_t g = (std::uint64_t{1} << 63) | 1;
    t.open_group(g, "#1", 0);
    Event a = ev(1, 0, "Build", "a", Outcome::executed, 5, 5, 0);
    a.running = true;
    t.apply(std::span(&a, 1), g);
    t.advance(900);
    t.close_group(g, 1000);
    check(t.running() == 0 && t.node(1).ev.outcome == Outcome::failed && t.node(1).ev.end_ns == 900 && !t.summary().running,
          "a request's end fails what still runs, where its time reached");
    check(inconsistent(t).empty(), inconsistent(t));
  }
  // A new run of the request under a view holding a node of the old tree (#939's review).
  {
    trace::Tree t;
    std::vector<Event> many;
    for (std::uint64_t k = 1; k <= 300; ++k) many.push_back(ev(k, k == 1 ? 0 : 1, "Pp", std::format("h{}", k), Outcome::executed, k, k + 1, 0));
    t.apply(many);
    trace::Browser b(t);
    b.resize(30, 120);
    b.set_live("building a.cpp", false);
    (void)b.handle(trace::Key{"G"});
    check(!b.following() && b.cursor() > 100, "the user went to the end");
    (void)b.pump([&] {
      t.reset();
      t.apply(std::span(many).first(3));
      return true;
    });
    check(b.cursor() == 0 && t.resets() == 1 && t.nodes().size() == 3, "a new tree: the view starts at its top");
    check(plain(b.frame()).contains("Pp(h1)"), "and draws it");
  }
  // The filter's is:running.
  {
    trace::Tree t;
    Event a = ev(1, 0, "Build", "a", Outcome::executed, 0, 0, 0), b = ev(2, 1, "Tu", "a", Outcome::reused, 5, 5, 0);
    a.running = true;
    t.apply(std::array{a, b});
    t.set_filter("is:running");
    check(t.rows().size() == 2 && t.node(t.rows()[0].node).ev.running, "is:running keeps the running cells (with what is under them)");
    t.set_filter("Tu is:running");
    check(t.rows().empty(), "and only them");
    t.set_filter("is:ex");
    check(t.rows().empty(), "a running cell has no outcome yet");
    t.set_filter("is:r");
    check(t.rows().size() == 2, "is:r is still reused (under its running parent)");
  }

  // Simulated requests, reported in orders the engine could report them in, and then in any order
  // at all: every step consistent, and the tree each ends with is the batches' tree.
  std::mt19937 rng(939);
  const auto origin = std::chrono::steady_clock::now();
  int compared = 0;
  for (int round = 0; round < 60; ++round) {
    const Sim sim(rng, origin);
    const auto order = sim.valid_order(rng);
    // The batches, as `--trace-engine` builds them after a build.
    trace::Tree batch;
    {
      trace::Collector c([&](std::span<const Event> b) { batch.add(b); }, false, origin);
      for (const auto& [start, e] : order)
        if (!start) c.event(e);
      c.finish();
    }
    // Live, in the engine's order, through `trace::Live`.
    trace::Live live(false, origin);
    for (const auto& [start, e] : order) start ? live.started(e) : live.event(e);
    std::vector<Event> events = live.take();
    check(live.take().empty(), "take empties the buffer");
    const auto run = [&](std::vector<Event> stream, std::string_view how) {
      trace::Tree t;
      std::size_t i = 0;
      while (i < stream.size()) {
        const std::size_t n = std::min(stream.size() - i, std::uniform_int_distribution<std::size_t>(1, 9)(rng));
        t.apply(std::span(stream).subspan(i, n));
        i += n;
        t.advance(i * 100);
        if (const std::string why = inconsistent(t); !why.empty()) {
          check(false, std::format("round {}, {}, after {} events: {}", round, how, i, why));
          return;
        }
        (void)trace::Browser(t).frame();  // draws at any step
      }
      t.settle();
      check(t.running() == 0, std::format("round {}, {}: nothing runs at the end", round, how));
      const std::string want = shown(batch), got = shown(t);
      check(got == want, std::format("round {}, {}: the live tree is the batches' tree", round, how));
      if (got != want) std::println(std::cerr, "--- batches:\n{}\n--- live:\n{}", want, got);
      ++compared;
    };
    run(events, "in the engine's order");
    std::vector<Event> shuffled = events;
    std::ranges::shuffle(shuffled, rng);
    run(shuffled, "in any order");
    run(std::vector(events.rbegin(), events.rend()), "backwards");
  }
  check(compared == 180, "every stream was compared");

  // Two requests at once, interleaved, each under its node (`--watch`, `ctl trace`).
  {
    const Sim a(rng, origin, 1), b(rng, origin, 100'000);
    trace::Tree batch, t;
    const std::uint64_t ga = (std::uint64_t{1} << 63) | 1, gb = (std::uint64_t{1} << 63) | 2;
    batch.open_group(ga, "#1", 0), batch.open_group(gb, "#2", 0);
    t.open_group(ga, "#1", 0), t.open_group(gb, "#2", 0);
    for (const auto& [sim, g] : {std::pair{&a, ga}, std::pair{&b, gb}}) {
      trace::Collector c([&, g](std::span<const Event> evs) { batch.add(evs, g); }, false, origin);
      for (const auto& [start, e] : sim->valid_order(rng))
        if (!start) c.event(e);
      c.finish();
    }
    trace::Live la(false, origin), lb(false, origin);
    for (const auto& [start, e] : a.valid_order(rng)) start ? la.started(e) : la.event(e);
    for (const auto& [start, e] : b.valid_order(rng)) start ? lb.started(e) : lb.event(e);
    std::vector<Event> ea = la.take(), eb = lb.take();
    std::ranges::shuffle(ea, rng);
    std::size_t i = 0, j = 0;
    while (i < ea.size() || j < eb.size()) {
      if (i < ea.size() && (j == eb.size() || rng() % 2)) t.apply(std::span(ea).subspan(i++, 1), ga);
      else t.apply(std::span(eb).subspan(j++, 1), gb);
      if (const std::string why = inconsistent(t); !why.empty()) {
        check(false, "two requests: " + why);
        break;
      }
    }
    batch.close_group(ga, 10), batch.close_group(gb, 10);
    t.close_group(ga, 10), t.close_group(gb, 10);
    check(shown(t) == shown(batch), "two requests interleaved: each request's tree is its batches'");
  }

  // Following (#939): the newest activity selected while it comes; a move of the user's stops it,
  // `f` brings it back, and the end of the build goes back to the top.
  {
    trace::Tree t;
    trace::Browser b(t);
    b.resize(30, 120);
    b.set_live("building a.cpp", false);
    std::vector<Event> pending;
    const auto feed = [&](Event e) {
      pending.push_back(std::move(e));
      return b.pump([&] {
        t.apply(pending);
        pending.clear();
        t.advance(1000);  // the running cells reach the timeline's end
        return true;
      });
    };
    Event build = ev(1, 0, "Build", "a.cpp", Outcome::executed, 0, 0, -1);
    build.running = true;
    feed(build);
    Event tu = ev(2, 1, "Tu", "a.cpp", Outcome::executed, 10, 10, 0);
    tu.running = true;
    feed(tu);
    for (std::uint64_t k = 0; k < 40; ++k) feed(ev(3 + k, 2, "Pp", std::format("h{}.h", k), Outcome::executed, 20 + k, 30 + k, 1));
    Event last = ev(50, 2, "Lower", "main", Outcome::executed, 100, 100, 2);
    last.running = true;
    feed(last);
    check(b.following(), "a live view follows");
    const auto rows = t.rows();
    check(t.node(rows[b.cursor()].node).ev.id == 50, std::format("the newest running cell is selected (row {})", b.cursor()));
    check(plain(b.frame()).contains("Lower(main) running"), "and in view, running");
    check(plain(b.frame()).contains("▶ 2 running") || plain(b.frame()).contains("▶ 3 running"), "the header counts the running cells");
    check(plain(b.frame()).contains("following"), "the status line says it follows");
    (void)b.handle(trace::Key{"k"});
    check(!b.following(), "a move of the user's stops following");
    const std::size_t held = b.cursor();
    Event more = ev(60, 2, "Lower", "f", Outcome::executed, 200, 200, 3);
    more.running = true;
    feed(more);
    check(b.cursor() == held, "not following: the selection stays");
    (void)b.handle(trace::Key{"f"});
    check(b.following() && t.node(t.rows()[b.cursor()].node).ev.id == 60, "f follows again, at once");
    (void)b.handle(trace::Mouse{trace::Mouse::Act::wheel_down, 0, 10, 10});
    check(!b.following(), "the wheel stops following");
    (void)b.handle(trace::Key{"f"});
    (void)b.handle(trace::Key{"s"});
    check(b.following(), "sorting keeps following: it keeps the node, not the row");
    (void)b.handle(trace::Key{"t"});
    (void)b.handle(trace::Key{"\r"});
    check(!b.following(), "a slice opened from the timeline is the user's choice: following stops");
    (void)b.handle(trace::Key{"f"});
    b.end_live("the build has ended");
    check(b.cursor() == 0 && !b.following() && plain(b.frame()).contains("the build has ended"), "the end goes back to the top, and says so");
  }
}

// Layout (#1119): what fits a width, and that the bars at the top and the bottom keep what matters
// at every width from 80, in every view, with the help or the filter prompt open.
void layout() {
  using trace::strip_sgr;
  check(trace::ellipsize("abcdef", 6) == "abcdef" && strip_sgr(trace::ellipsize("abcdef", 4)) == "abc…", "a cut ends in …");
  check(strip_sgr(trace::ellipsize("ab    ", 3)) == "ab ", "only blanks past the edge: no …");
  check(trace::columns(trace::ellipsize("\x1b[1mé\x1b[0mabcd", 3)) == 3, "it counts columns, not bytes or SGR");
  const std::vector<trace::Piece> ps{{"a", 1}, {"bbbb", 3}, {"cc", 2}};
  check(strip_sgr(trace::pack(ps, 20)) == "a  bbbb  cc", "pieces that fit, in order");
  check(strip_sgr(trace::pack(ps, 8)) == "bbbb  cc", "the lowest priority goes first");
  check(strip_sgr(trace::pack(ps, 5)) == "bbbb" && strip_sgr(trace::pack(ps, 2)) == "b…", "down to the most important, cut");
  check(trace::spread("left", "right", 12) == "left   right", "right-aligned on one line");
  check(strip_sgr(trace::spread("a long left part", "right", 12)) == "a lon… right", "the left gives way first");
  check(trace::home_relative("watching /home/u/p", "/home/u") == "watching ~/p" && trace::home_relative("(/home/u)", "/home/u") == "(~)",
        "a path under home as ~");
  check(trace::home_relative("/home/user2/p", "/home/u") == "/home/user2/p", "not a mere prefix");
  check(trace::ellipsize_path("watching /a/b/c/project", 18) == "watching …/project" && trace::ellipsize_path("watching /a/b", 18) == "watching /a/b",
        "a long path keeps its end");
  check(trace::wrap("/a/b/ccc/ddd", 8) == std::vector<std::string>{"/a/b/", "ccc/ddd"}, "a path wraps after a /");
  check(trace::wrap("abcdefghij", 4) == std::vector<std::string>{"abcd", "efgh", "ij"}, "a word longer than the line breaks in it");
  check(trace::wrap("watching 1 file: x", 12) == std::vector<std::string>{"watching 1 ", "file: x"}, "words wrap at a space, kept");

  trace::Tree t;
  t.add(browser_events());
  trace::Browser b(t);
  b.set_live("watching /a/path/long/enough/that/it/never/fits/next/to/the/tabs/on/a/screen/of/eighty/columns", false);
  for (std::size_t width : {80uz, 81uz, 99uz, 100uz, 120uz, 160uz, 200uz, 237uz}) {
    b.resize(30, width);
    for (const char* view : {"tree", "timeline", "histogram"}) {
      (void)b.handle(trace::Key{std::string(view) == "histogram" ? "b" : std::string(view) == "timeline" ? "t" : "g"});
      for (const char* open : {"", "?", "/"}) {
        if (*open) (void)b.handle(trace::Key{open});
        const trace::Frame f = b.frame();
        const std::string what = std::format("{} at {}{}", view, width, *open ? std::format(" with {}", open) : "");
        check(f.lines.size() == 30, what + ": the screen's height");
        for (const std::string& l : f.lines) check(trace::columns(l) == width, what + ": every line the screen's width");
        const std::string title = strip_sgr(f.lines.front()), foot = strip_sgr(f.lines.back());
        check(title.starts_with(" ycxx trace ") && title.contains("● live") && title.contains("following"), what + ": the title bar: " + title);
        check(f.lines.front().contains(std::format("\x1b[1;7m {} ", view)), what + ": the current tab lit");
        if (*open == '/') check(foot.starts_with(" / ") && foot.contains("Esc cancel"), what + ": the prompt in the footer: " + foot);
        else check(foot.contains("? help  q quit") && !foot.contains("…"), what + ": help and quit on the right, no key cut: " + foot);
        if (!*open) {
          const std::string toggles = strip_sgr(f.lines[b.header_height() - 1]);
          check(toggles.contains("/ filter") && toggles.contains("w all workers"), what + ": the toggles: " + toggles);
        }
        if (*open == '?') (void)b.handle(trace::Key{"q"});  // closes the help, quits nothing
        if (*open == '/') (void)b.handle(trace::Key{"\x1b"});
      }
      if (std::string(view) != "tree") (void)b.handle(trace::Key{std::string(view) == "histogram" ? "b" : "t"});
    }
  }
  // A toggle off its default stands out, and a click on its chip toggles it.
  b.resize(30, 120);
  (void)b.handle(trace::Key{"r"});
  check(b.frame().lines[b.header_height() - 1].contains("\x1b[1;33mreused hidden"), "reused hidden, in bold yellow");
  const std::size_t y = b.header_height() - 1;
  std::size_t x = 0;
  for (; x < 120; ++x)
    if (b.hit(x, y) == trace::Hit{trace::Hit::Area::toggle, std::to_underlying(trace::Toggle::reused)}) break;
  (void)b.handle(click(x, y));
  (void)b.handle(click(x, y, trace::Mouse::Act::release));
  check(!t.hide_reused() && strip_sgr(b.frame().lines[y]).contains("reused shown"), "a click on the chip shows them again");
  // Review fixes: on a narrow screen the prompt shows what is typed and its cursor stays on it;
  // a tab the title bar cut takes no click; the help pages by what it shows.
  b.resize(24, 30);
  (void)b.handle(trace::Key{"/"});
  for (char c : std::string_view("abc")) (void)b.handle(trace::Key{std::string(1, c)});
  {
    const trace::Frame f = b.frame();
    check(strip_sgr(f.lines.back()).starts_with(" / abc") && f.cursor && f.cursor->second == 6, "a narrow prompt keeps what is typed: " + strip_sgr(f.lines.back()));
  }
  for (char c : std::string_view("defghijklmnopqrstuvwxyz0123456789")) (void)b.handle(trace::Key{std::string(1, c)});
  {
    const trace::Frame f = b.frame();
    const std::string foot = strip_sgr(f.lines.back());
    check(foot.starts_with(" / …") && foot.contains("6789") && f.cursor && f.cursor->second < 30, "a long filter shows its end: " + foot);
  }
  (void)b.handle(trace::Key{"\x1b"});
  check(b.hit(29, 0) != trace::Hit{trace::Hit::Area::tab, std::to_underlying(trace::View::histogram)}, "a tab cut off the title bar is no target");
  check(b.hit(14, 0) == trace::Hit{trace::Hit::Area::tab, std::to_underlying(trace::View::tree)}, "one drawn is");
  b.resize(24, 80);
  (void)b.handle(trace::Key{"?"});
  (void)b.handle(trace::Key{"\x1b[6~"});
  check(strip_sgr(b.frame().lines[1]).contains("keys 21–"), "PgDn in the help goes to the line after the last shown: " + strip_sgr(b.frame().lines[1]));
  (void)b.handle(trace::Key{"x"});
  // The help scrolls when it is taller than the screen, and says where it is.
  b.resize(24, 80);
  (void)b.handle(trace::Key{"?"});
  const std::string top = strip_sgr(b.frame().lines[1]);
  (void)b.handle(trace::Key{"j"});
  check(top.contains("keys 1–") && strip_sgr(b.frame().lines[1]).contains("keys 2–"), "j scrolls the help: " + top);
  (void)b.handle(trace::Key{"x"});
  check(!strip_sgr(b.frame().lines[1]).contains("keys"), "any other key closes it");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::println(std::cerr, "usage: unit_trace <golden> [--update]");
    return 2;
  }
  const bool update = argc > 2 && std::string_view(argv[2]) == "--update";
  golden(argv[1], update);
  nested_compiler();
  cancellation();
  input();
  browser(std::filesystem::path(argv[1]).parent_path() / "browser.golden", update);
  copying();
  collector();
  tree();
  lines();
  memory();
  spans();
  histograms(std::filesystem::path(argv[1]).parent_path() / "histogram.golden", update);
  live();
  layout();
  return ycxx::unit::result("unit.trace");
}
