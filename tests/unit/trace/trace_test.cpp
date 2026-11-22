// The engine trace (#245): the stream of a small unit built cold, again, and after a whitespace
// edit, against a golden file (colors and times off); and the model behind the browser: the
// collector's batches, the tree's rows, filters and summary, and the timeline.
//
//   unit_trace <golden>            checks
//   unit_trace <golden> --update   rewrites the golden file
import std;
import ycxx.engine;
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
  // Rows: 0 header, 1 status, 2 column headers, 3.. the tree; the divider at 72 (3/5 of 120).
  check(b.left_width() == 72, "the tree takes 3/5");
  check(b.hit(0, 0) == H{H::Area::header}, "the header");
  check(b.hit(2, 1) == H{H::Area::tab, std::to_underlying(trace::View::tree)}, "the tree's tab");
  check(b.hit(8, 1) == H{H::Area::tab, std::to_underlying(trace::View::timeline)}, "the timeline's tab");
  check(b.hit(30, 1) == H{H::Area::status}, "the rest of the status line");
  check(b.hit(1, 2) == H{H::Area::column, std::to_underlying(trace::Column::cell)}, "the cell's header");
  check(b.hit(40, 2) == H{H::Area::column, std::to_underlying(trace::Column::time)}, "the time header");
  check(b.hit(71, 2) == H{H::Area::column, std::to_underlying(trace::Column::retained)}, "the retains header");
  check(b.hit(0, 3) == H{H::Area::marker, 0} && b.hit(5, 3) == H{H::Area::row, 0}, "the first row's marker, and the row");
  check(b.hit(2, 4) == H{H::Area::marker, 1} && b.hit(0, 4) == H{H::Area::row, 1}, "a nested row's marker is indented");
  check(b.hit(2, 5) == H{H::Area::row, 2}, "a leaf has no marker");
  check(b.hit(5, 20) == H{}, "below the last row");
  check(b.hit(72, 10) == H{H::Area::divider} && b.hit(73, 10) == H{H::Area::divider} && b.hit(71, 10) != H{H::Area::divider}, "the divider, two columns wide to grab");
  check(b.hit(90, 2) == H{H::Area::title} && b.hit(90, 3) == H{H::Area::detail, 0} && b.hit(72, 2) == H{H::Area::divider}, "the pane's title and first line; the divider in the title row");
  check(b.hit(5, 23) == H{H::Area::footer} && b.hit(5, 22) == H{}, "the footer, on the last row");

  press(2, 7);  // FunctionIr(square)'s marker folds it (depth 1 starts open)
  step("a click on a marker folds the row, and selects it");
  check(b.cursor() == 4 && !t.node(4).expanded, "selected and folded");
  press(2, 7);
  check(t.node(4).expanded, "a second click opens it again");
  press(2, 7);
  press(10, 4, milliseconds{1000});
  press(10, 4, milliseconds{1200});  // a double click folds Pp
  step("a double click folds a row");
  check(b.cursor() == 1 && !t.node(1).expanded, "Pp is folded");
  press(10, 4, milliseconds{2000});
  press(10, 4, milliseconds{2500});  // too slow: two clicks
  check(!t.node(1).expanded, "two slow clicks fold nothing");
  press(40, 2);
  step("a click on the time header sorts by time");
  check(t.order() == trace::Tree::Order::time, "by time");
  press(40, 2);
  check(t.order() == trace::Tree::Order::start, "a second click goes back to start order");
  // Columns: cell 0–34, time 36–43, worker 45–51, alloc 53–61, retains 63–71.
  press(58, 2);
  check(t.order() == trace::Tree::Order::alloc, "the alloc header");
  press(66, 2);
  check(t.order() == trace::Tree::Order::retained, "the retains header");
  press(58, 2);
  press(48, 2);
  check(t.order() == trace::Tree::Order::alloc, "the worker header does not sort");
  press(1, 2);
  check(t.order() == trace::Tree::Order::start, "the cell header: start order");

  // The pane: a click focuses it, the wheel scrolls it, keys then scroll it.
  press(10, 5);  // FunctionIr(square), whose value is long
  press(90, 8);
  check(b.focus() == trace::Focus::detail, "a click on the pane focuses it");
  (void)b.handle(click(90, 8, trace::Mouse::Act::wheel_down));
  (void)b.handle(click(90, 8, trace::Mouse::Act::wheel_down));
  step("the wheel scrolls the pane under it");
  check(b.detail_scroll() == 2 * trace::Browser::wheel_step, "two notches");
  (void)b.handle(trace::Key{"j"});
  check(b.detail_scroll() == 2 * trace::Browser::wheel_step + 1 && b.cursor() == 2, "j scrolls the focused pane, not the tree");
  (void)b.handle(trace::Key{"\x1b"});
  (void)b.handle(trace::Key{"j"});
  check(b.focus() == trace::Focus::tree && b.cursor() == 3, "Escape gives the keys back to the tree");

  // The divider drags, within bounds.
  (void)b.handle(click(72, 10));
  check(b.dragging(), "a press on the divider starts a drag");
  (void)b.handle(click(60, 10, trace::Mouse::Act::drag));
  (void)b.handle(click(60, 10, trace::Mouse::Act::release));
  step("the divider dragged to column 60");
  check(b.left_width() == 60 && !b.dragging(), "the tree is 60 wide");
  (void)b.handle(click(74, 10));
  check(!b.dragging(), "the divider has moved: column 74 is the pane");
  (void)b.handle(click(60, 10));
  (void)b.handle(click(2, 10, trace::Mouse::Act::drag));
  (void)b.handle(click(2, 10, trace::Mouse::Act::release));
  check(b.left_width() == 24, "a pane keeps 24 columns");
  check(b.hit(23, 2) == H{H::Area::column, std::to_underlying(trace::Column::cell)}, "a narrow tree drops its columns: the cell's is all");
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
  press(5, 3);  // the tree takes the keys again (the last click was on the pane)
  (void)b.handle(trace::Key{"g"});
  (void)b.handle(click(5, 5, trace::Mouse::Act::wheel_down));
  (void)b.frame();
  check(b.top() == 3 && b.cursor() == 3, std::format("the view moved 3 rows, the selection with it ({} {})", b.top(), b.cursor()));
  (void)b.handle(click(5, 5, trace::Mouse::Act::wheel_down));
  (void)b.handle(click(5, 5, trace::Mouse::Act::wheel_down));
  (void)b.frame();
  check(b.top() == 3, "and stops at the last page");
  (void)b.handle(click(5, 5, trace::Mouse::Act::wheel_up));
  (void)b.frame();
  check(b.top() == 0 && b.cursor() == 3, "up again; the selection stays where it was, in view");
  b.resize(24, 120);

  // A narrow screen: the summary wraps onto more lines rather than run off, and everything
  // below moves down with it.
  b.resize(24, 60);
  step("a narrow screen: the summary wraps");
  check(b.header_height() == 3, std::format("three summary lines at 60 columns: {}", b.header_height()));
  check(b.hit(2, 3) == H{H::Area::tab, std::to_underlying(trace::View::tree)}, "the status line moves down");
  check(b.hit(5, 5) == H{H::Area::row, 0}, "and the rows");
  for (const std::string& l : b.frame().lines) check(trace::columns(l) == 60, "every line is the screen's width");
  b.resize(24, 120);
  check(b.header_height() == 1, "one line at 120");

  // The timeline: its tab, a click on a slice, a double click opens it in the tree.
  press(8, 1);
  check(b.view() == trace::View::timeline, "the timeline's tab");
  step("the timeline");
  // Lanes from row 3 (row 2 the axis): main, w0, w1, w2, w3; slices from column 9.
  check(b.hit(20, 6) == H{H::Area::lane, 3, 11}, "a slice of w2's lane");
  press(9 + 30, 6, milliseconds{5000});
  check(b.lane() == 3 && b.time() == 30, "a click moves the cursor there");
  (void)b.handle(click(9 + 40, 6, trace::Mouse::Act::wheel_down));
  check(b.time() == 31, "the wheel scrubs");
  press(9 + 30, 6, milliseconds{6000});
  press(9 + 30, 6, milliseconds{6100});
  check(b.view() == trace::View::tree, "a double click opens the slice in the tree");
  check(t.node(t.rows()[b.cursor()].node).ev.kind == "FunctionGen", "at the cell w2 was in");
  press(2, 1);
  check(b.view() == trace::View::tree, "the tree's tab");

  // Review fixes: a double click is twice on the same thing; a lost release ends no drag; the pane
  // gives the keys back when it is hidden; a marker cut off by the columns is not a target.
  (void)b.handle(trace::Key{"t"});
  press(9 + 10, 6, milliseconds{7000});
  press(9 + 50, 6, milliseconds{7100});
  check(b.view() == trace::View::timeline && b.time() == 50, "two clicks on different slices of a lane are no double click");
  (void)b.handle(trace::Key{"t"});
  (void)b.handle(click(72, 10));  // a press on the divider whose release is lost
  (void)b.handle(click(10, 5));   // the next press, on a row
  (void)b.handle(click(30, 5, trace::Mouse::Act::drag));
  check(b.left_width() == 72 && !b.dragging(), "a lost release: the next press ends the drag");
  press(90, 8);
  check(b.focus() == trace::Focus::detail, "the pane has the keys");
  b.resize(24, 90);
  const std::size_t at = b.cursor();
  (void)b.handle(trace::Key{"j"});
  check(b.focus() == trace::Focus::tree && b.cursor() == at + 1, "a hidden pane gives the keys back to the tree");
  b.resize(24, 120);
  press(5, 3);

  // Help closes on a click; a click closes the filter prompt and keeps what was typed.
  (void)b.handle(trace::Key{"?"});
  press(5, 5);
  check(b.frame().lines[3].find("What the engine") == std::string::npos, "a click closes the help");
  (void)b.handle(trace::Key{"/"});
  for (char c : std::string_view("functiong")) (void)b.handle(trace::Key{std::string(1, c)});
  press(5, 3);
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
    check(db.hit(32, 2 + 1 + 16) == H{H::Area::marker, 16}, "depth 16: the marker fits (32–33)");
    check(db.hit(34, 2 + 1 + 17) == H{H::Area::row, 17}, "depth 17: the marker (34–35) is cut off, so the row");
    check(db.hit(36, 2 + 1 + 18) == H{H::Area::row, 18}, "depth 18: a click in the time column is the row");
  }

  const std::string text = got;
  if (update) {
    std::ofstream(golden_path, std::ios::binary) << text;
    std::println("updated {}", golden_path.display_string());
    return;
  }
  std::ifstream in(golden_path, std::ios::binary);
  const std::string want{std::istreambuf_iterator<char>(in), {}};
  check(text == want, "the browser's frames equal browser.golden (rerun with --update after a deliberate change)");
  if (text != want) std::println(std::cerr, "--- got:\n{}", text);
}

// Selection and copy (#636): what each way of copying puts on the clipboard, exactly.
void copying() {
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
  // row r on screen line 3 + r.
  (void)act(10, 4, M::press);
  (void)act(10, 5, M::drag);
  (void)act(10, 6, M::drag);
  trace::Action a = act(10, 6, M::release);
  check(a.copy == "* Pp(a.cpp) executed 190µs w1 alloc 20.0KiB, retains 5.0KiB\n"
                  ".   FileText(a.cpp) input w1 retains 300B\n"
                  ".   FileText(x.h) input w1 retains 200B\n",
        std::format("a drag over three rows copies them as the stream prints them: {:?}", a.copy.value_or("(nothing)")));
  check(trace::strip_sgr(b.frame().lines[1]).contains("copied 3 rows"), "the status line says so");
  (void)act(10, 9, M::press);
  a = act(10, 9, M::release);
  check(!a.copy && b.cursor() == 6, "a click copies nothing");
  check(!trace::strip_sgr(b.frame().lines[1]).contains("copied"), "and the note goes");
  (void)act(10, 9, M::press);  // Codegen, whose error starts below it, and Optimized, where it starts
  (void)act(10, 10, M::drag);
  a = act(10, 10, M::release);
  check(a.copy == "* Codegen(main) executed 350µs w0 alloc 500B, 1.2KiB in all, retains 100B (error below: a.cpp:2:5: error: no instruction for this)\n"
                  "*   Optimized({1, aarch64_macos, 0}) executed 240µs w3 alloc 700B, retains 64B ← error here\n"
                  "      error: a.cpp:2:5: error: no instruction for this\n",
        std::format("errors copy as the stream shows them: {:?}", a.copy.value_or("(nothing)")));

  // The pane: a drag over the value's text copies those columns. Its lines start at screen line 3;
  // FunctionIr's value starts at pane line 20 (`func {`); the wheel scrolls 6 lines down.
  (void)act(10, 7, M::press);
  (void)act(10, 7, M::release);
  (void)act(90, 10, M::wheel_down);
  (void)act(90, 10, M::wheel_down);
  (void)b.frame();
  check(b.detail_scroll() == 6, "scrolled");
  (void)act(73 + 3, 3 + 21 - 6, M::press);
  (void)act(73 + 6, 3 + 22 - 6, M::drag);
  a = act(73 + 6, 3 + 22 - 6, M::release);
  check(a.copy == "%0 = const.i32 0\n  %1 =", std::format("a drag over the pane copies the text between its ends: {:?}", a.copy.value_or("(nothing)")));
  check(b.focus() == trace::Focus::detail, "and gives the pane the keys");
  // Backwards, and from the margin: the margin is never text.
  (void)act(73 + 6, 3 + 22 - 6, M::press);
  (void)act(72 + 1, 3 + 21 - 6, M::drag);
  a = act(72 + 1, 3 + 21 - 6, M::release);
  check(a.copy == "  %0 = const.i32 0\n  %1 =", std::format("backwards from the margin: {:?}", a.copy.value_or("(nothing)")));

  // V in the pane: whole lines from the top one shown; j extends; y copies.
  (void)key("V");
  (void)key("j");
  (void)key("j");
  check(trace::strip_sgr(b.frame().lines[1]).contains("V: j/k extend"), "V says what it does");
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
  check(a.copy && a.copy->ends_with("memory\nalloc    4.0KiB in 128 allocations\npeak     4.0KiB\nretains  1.0KiB\nin all   29.1KiB allocated, 6.9KiB retained\n\nno value shown for this kind\n"),
        "its memory, and that it has no value");
  (void)key("E");  // fold everything: Y copies what is folded too
  a = key("Y");
  const auto lines = a.copy ? std::ranges::count(*a.copy, '\n') : 0;
  check(lines == 10 && a.copy->starts_with("* Tu(a.cpp) executed 900µs w0 alloc 4.0KiB, 29.1KiB in all, retains 1.0KiB\n"),
        std::format("Y copies the whole subtree, folded or not ({} lines: 9 cells and an error line)", lines));
  check(trace::strip_sgr(b.frame().lines[1]).contains("copied the subtree (9 cells)"), "and says how much");

  // A selection is of cells, not row numbers, and goes when its rows or its pane change under it.
  const auto status = [&] {
    const trace::Frame f = b.frame();
    return trace::strip_sgr(f.lines[b.header_height()]);
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
  (void)act(10, 7, M::press);  // FunctionIr, whose pane is long
  (void)act(10, 7, M::release);
  (void)act(90, 6, M::press);
  (void)act(90, 6, M::release);
  (void)key("V");
  (void)key("j");
  (void)key("h");  // the keys go back to the tree: the pane's selection goes with them
  (void)key("j");
  a = key("y");
  check(status().contains("copied the cell") && a.copy && a.copy->starts_with("FunctionGen"), "leaving the pane drops its selection");
  (void)act(10, 7, M::press);
  (void)act(10, 7, M::release);
  (void)act(73 + 3, 6, M::press);
  (void)act(73 + 8, 6, M::drag);
  (void)key("<");  // the pane is wider now: its lines are others
  a = key("y");
  check(status().contains("copied the cell"), "a resized pane drops a selection");
  (void)key(">");
  // A click on the long pane's last row (where it says which lines it shows) scrolls nothing.
  (void)b.frame();
  const std::size_t scroll = b.detail_scroll();
  (void)act(90, 3 + 19, M::press);
  (void)act(90, 3 + 19, M::release);
  check(b.detail_scroll() == scroll, "a click on the pane's last row does not scroll it");
  // A drag over a blank line copies nothing, and says so.
  (void)act(73 + 2, 3 + 2 - b.detail_scroll(), M::press);
  (void)act(73 + 9, 3 + 2 - b.detail_scroll(), M::drag);
  a = act(73 + 9, 3 + 2 - b.detail_scroll(), M::release);
  check(!a.copy && status().contains("nothing to copy"), std::format("an empty selection copies nothing: {}", status()));

  // A live feed adding rows above keeps a selection on its cells.
  b.set_live("test");
  (void)act(10, 4, M::press);  // Pp (newest first: the one request is still on top)
  (void)act(10, 6, M::drag);
  (void)b.pump([&] {
    Event top = ev(100, 0, "Tu", "b.cpp", Outcome::executed, 950'000, 990'000);
    t.add(std::vector<Event>{top});
    return true;
  });
  a = act(10, 7, M::release);
  check(a.copy && a.copy->starts_with("* Pp(a.cpp)") && std::ranges::count(*a.copy, '\n') == 3,
        std::format("the rows stay the cells that were selected: {:?}", a.copy.value_or("(nothing)")));
  // A drag held past the bottom edge keeps scrolling on ticks; the wheel moves its end too.
  b.resize(10, 120);  // 6 tree rows of 11
  (void)b.frame();
  const std::size_t y0 = b.header_height() + 2;  // the first tree row
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
  check(std::ranges::count_if(all, [](const Event& e) { return !e.span.file.empty(); }) > 10, "most cells of a unit have spans");

  // An edit that moves the function: the second build's cells (cut off or verified) say where it
  // is now.
  got.clear();
  s.set_source(tu, "spans.cpp", "\n\nint square(int x) {\n  return x * x;\n}\nint main() { return square(3) - 9; }\n", "spans.o");
  (void)s.get<pl::Tu>(tu, &c);
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
  hs.set_source(htu, "m.cpp", main_text, "m.o", dir.native_encoded_string());
  (void)hs.get<pl::Tu>(htu, &c);
  c.finish();
  const SourceSpan cube = at("FunctionIr(cube)");
  check(cube.file.ends_with("h.h") && cube.line == 2 && cube.column == 1 && cube.end_line == 2 && cube.end_column == 44,
        std::format("a header's function, in the header: {}", trace::span_text(cube)));
  const pl::TuId editor{1u << 31};
  ycxx::pp::Config editor_config;
  editor_config.view = ycxx::pp::View::editor;
  hs.db().set<ycxx::pp::Buffer>(ycxx::pp::path_id((dir / "h.h").lexically_normal().generic_native_encoded_string()), "// a\n// b\n" + header);
  got.clear();
  hs.set_source(editor, "m.cpp", "", "m.o", dir.native_encoded_string(), editor_config);
  (void)hs.get<pl::Tu>(editor, &c);
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::println(std::cerr, "usage: unit_trace <golden> [--update]");
    return 2;
  }
  const bool update = argc > 2 && std::string_view(argv[2]) == "--update";
  golden(argv[1], update);
  input();
  browser(std::filesystem::path(argv[1]).parent_path() / "browser.golden", update);
  copying();
  collector();
  tree();
  lines();
  memory();
  spans();
  return ycxx::unit::result("unit.trace");
}
