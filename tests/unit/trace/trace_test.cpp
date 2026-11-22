// The engine trace (#245): the stream of a small unit built cold, again, and after a whitespace
// edit, against a golden file (colors and times off); and the model behind the browser: the
// collector's batches, the tree's rows, filters and summary, and the timeline.
//
//   unit_trace <golden>            checks
//   unit_trace <golden> --update   rewrites the golden file
import std;
import ycxx.engine;
import ycxx.pipeline;
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
  (void)s.get<pl::Tu>(tu, &c);
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
  (void)s.get<pl::Tu>(tu, &c);
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
  // An untraced build reads no span: `trace_span` runs only from `Db::trace`. (Its cost when off is
  // none by construction; this checks a traced build's spans are there, and nothing else needs one.)
  check(std::ranges::count_if(all, [](const Event& e) { return !e.span.file.empty(); }) > 10, "most cells of a unit have spans");

  check(trace::span_start(SourceSpan{"a.cpp", 3, 5, 7, 1}) == "a.cpp:3:5" && trace::span_text(SourceSpan{"a.cpp", 3, 5, 7, 1}) == "a.cpp:3:5–7:1" &&
            trace::span_text(SourceSpan{"a.cpp", 3, 5, 3, 12}) == "a.cpp:3:5–12" && trace::span_text(SourceSpan{"a.cpp"}) == "a.cpp" &&
            trace::span_text(SourceSpan{}).empty(),
        "span texts");
  const SourceSpan f{"/src/dir/foo.cpp", 120, 1, 140, 1};
  check(trace::span_touches(f, "foo.cpp:120") && trace::span_touches(f, "dir/foo.cpp:130") && trace::span_touches(f, "/src/dir/foo.cpp") &&
            !trace::span_touches(f, "foo.cpp:141") && !trace::span_touches(f, "oo.cpp:120") && !trace::span_touches(f, "bar.cpp"),
        "a span touches a line of its file, by a path's tail");
  check(trace::span_touches(SourceSpan{"foo.cpp"}, "foo.cpp:9") && !trace::span_touches(SourceSpan{}, "foo.cpp"), "a whole file touches every line");

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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::println(std::cerr, "usage: unit_trace <golden> [--update]");
    return 2;
  }
  golden(argv[1], argc > 2 && std::string_view(argv[2]) == "--update");
  collector();
  tree();
  lines();
  memory();
  spans();
  return ycxx::unit::result("unit.trace");
}
