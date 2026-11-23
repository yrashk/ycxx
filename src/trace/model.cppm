// The trace as values (#245): one `Event` per cell a request settled, the line both modes render,
// and the browser's model: a tree with expansion, a filter by text, outcome or worker, the summary
// header, and the timeline of the pool's workers. Memory (#274): each event's own bytes, sums over
// its subtree, sums per kind, and a build's totals. Nothing here touches a terminal.
export module ycxx.trace:model;

import std;
import ycxx.engine;

export namespace ycxx::trace {

using engine::Outcome;
inline constexpr std::size_t outcome_count = 9;

// An engine event as it crosses the daemon socket: times in nanoseconds since the trace began.
struct Event {
  std::uint64_t id = 0;
  std::uint64_t parent = 0;
  std::uint32_t depth = 0;  // in the request's tree, set by the collector
  std::string kind;
  std::string key;
  Outcome outcome = Outcome::executed;
  std::uint64_t start_ns = 0;
  std::uint64_t end_ns = 0;
  std::int32_t worker = -1;  // the pool worker; -1 the requesting thread
  std::int32_t end_worker = -1;
  std::string value;   // the kind's `show`, when values were asked for
  std::string errors;  // the errors its value holds, one a line; empty: none
  std::uint32_t repeat = 1;  // identical instants in a row under one parent, shown once (`×3`)
  std::uint64_t cell = 0;    // the cell's identity: equal in every event of one cell
  // Memory (#274), as `engine::TraceEvent` has it: the frame's own allocations and their count,
  // its net bytes' high-water mark, and what the cell retains.
  std::uint64_t alloc_bytes = 0;
  std::uint64_t allocs = 0;
  std::int64_t peak_bytes = 0;
  std::int64_t retained_bytes = 0;
  engine::SourceSpan span;  // where in the source the cell is (#638); empty file: none
  // A live trace's start of a frame (#939): it is running, and its outcome, end and memory come
  // with its end. A running event's `end_ns` is its start until a live view moves it to now.
  bool running = false;
  bool operator==(const Event&) const = default;
};

// `a.cpp:3:5`, or `a.cpp` for a whole file: a span's start, as an editor opens it.
std::string span_start(const engine::SourceSpan& s) {
  if (s.file.empty()) return {};
  return s.line == 0 ? s.file : std::format("{}:{}:{}", s.file, s.line, s.column);
}
// `a.cpp:3:5–7:1`, `a.cpp:3:5–12` on one line, or `a.cpp`.
std::string span_text(const engine::SourceSpan& s) {
  if (s.file.empty() || s.line == 0) return span_start(s);
  if (s.end_line == s.line) return std::format("{}–{}", span_start(s), s.end_column);
  return std::format("{}–{}:{}", span_start(s), s.end_line, s.end_column);
}
// Whether a span touches `where`: `file` or `file:line`, the file as given or a path ending in it
// at a `/` (`at:a.cpp:12`). A whole-file span touches every line of its file.
bool span_touches(const engine::SourceSpan& s, std::string_view where) {
  if (s.file.empty() || where.empty()) return false;
  // `file:line` or `file:line:column` (as `span_start` writes it): the line counts.
  std::optional<std::uint32_t> line;
  for (int i = 0; i < 2; ++i) {
    const std::size_t colon = where.rfind(':');
    if (colon == std::string_view::npos) break;
    std::uint32_t n = 0;
    const std::string_view digits = where.substr(colon + 1);
    const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (ec != std::errc{} || p != digits.data() + digits.size()) break;
    line = n;
    where = where.substr(0, colon);
  }
  const std::string_view file = s.file;
  // `=path`: that path exactly (a histogram's bar); else the path or one ending in it at a `/`.
  if (where.starts_with('=')) {
    if (file != where.substr(1)) return false;
  } else if (file != where && !(file.ends_with(where) && file[file.size() - where.size() - 1] == '/')) {
    return false;
  }
  return !line || s.line == 0 || (*line >= s.line && *line <= std::max(s.line, s.end_line));
}

// A build's memory beyond its events (#274): the process's bytes when it ended and their
// high-water mark while it ran (other builds the daemon ran at the same time included).
struct BuildMemory {
  std::int64_t process_current = 0;
  std::int64_t process_peak = 0;
  bool operator==(const BuildMemory&) const = default;
};

// `ycxx::pipeline::Tu` or `Tu@ycxx.pipeline` (a kind attached to a module) -> `Tu`.
std::string short_kind(std::string_view k) {
  k = k.substr(0, k.find('@'));
  const std::size_t angle = k.find('<');
  const std::size_t colons = k.substr(0, angle).rfind("::");
  return std::string(colons == std::string_view::npos ? k : k.substr(colons + 2));
}

constexpr std::string_view outcome_name(Outcome o) {
  switch (o) {
    case Outcome::executed: return "executed";
    case Outcome::cutoff: return "cutoff";
    case Outcome::verified: return "verified";
    case Outcome::reused: return "reused";
    case Outcome::durable: return "durable";
    case Outcome::input: return "input";
    case Outcome::recomputed: return "recomputed";
    case Outcome::waited: return "waited";
    case Outcome::failed: return "failed";
  }
  return "?";
}
// What each outcome means, for the browser's legend.
constexpr std::string_view outcome_meaning(Outcome o) {
  switch (o) {
    case Outcome::executed: return "ran, value changed";
    case Outcome::cutoff: return "ran, value unchanged: early cutoff";
    case Outcome::verified: return "green after re-checking its dependencies";
    case Outcome::reused: return "green: already verified at this revision";
    case Outcome::durable: return "green: nothing as durable changed";
    case Outcome::input: return "an input read for the first time";
    case Outcome::recomputed: return "computed privately: another frame owned it";
    case Outcome::waited: return "another frame owned it: waited for it";
    case Outcome::failed: return "left without a value";
  }
  return "?";
}
// One glyph per outcome, for the stream's margin and the timeline.
constexpr std::string_view outcome_glyph(Outcome o, bool unicode) {
  switch (o) {
    case Outcome::executed: return unicode ? "●" : "*";
    case Outcome::cutoff: return unicode ? "◐" : "~";
    case Outcome::verified: return unicode ? "✓" : "+";
    case Outcome::reused: return unicode ? "○" : "=";
    case Outcome::durable: return unicode ? "◇" : "#";
    case Outcome::input: return unicode ? "·" : ".";
    case Outcome::recomputed: return unicode ? "◆" : "%";
    case Outcome::waited: return unicode ? "⧗" : "&";
    case Outcome::failed: return unicode ? "✗" : "!";
  }
  return "?";
}
// SGR parameters: executed bold green, cutoff yellow, verified green, reused light gray (it did
// nothing),
// durable blue, inputs dim, recomputed bright yellow, waited cyan, failed bold magenta. A cell
// whose value holds errors is red whatever its outcome (`event_sgr`).
inline constexpr std::string_view error_sgr = "1;31";
inline constexpr std::string_view error_here_sgr = "1;97;41";
constexpr std::string_view outcome_sgr(Outcome o) {
  switch (o) {
    case Outcome::executed: return "1;32";
    case Outcome::cutoff: return "33";
    case Outcome::verified: return "32";
    case Outcome::reused: return "37";
    case Outcome::durable: return "34";
    case Outcome::input: return "90";
    case Outcome::recomputed: return "93";
    case Outcome::waited: return "36";
    case Outcome::failed: return "1;35";
  }
  return "0";
}
std::optional<Outcome> outcome_named(std::string_view s) {
  for (std::uint8_t i = 0; i < outcome_count; ++i) {
    if (outcome_name(static_cast<Outcome>(i)).starts_with(s) && !s.empty()) return static_cast<Outcome>(i);
  }
  return std::nullopt;
}

// A cell as shown, running or settled (#939): its glyph, name and colors. A running cell is bold
// orange, `▶`.
inline constexpr std::string_view running_sgr = "1;38;5;208";
constexpr std::string_view state_glyph(const Event& e, bool unicode) { return e.running ? (unicode ? "▶" : ">") : outcome_glyph(e.outcome, unicode); }
constexpr std::string_view state_name(const Event& e) { return e.running ? "running" : outcome_name(e.outcome); }
constexpr std::string_view state_meaning(const Event& e) { return e.running ? "started, not ended yet" : outcome_meaning(e.outcome); }

// `812ns`, `41µs`, `1.24ms`, `3.40s`.
std::string format_duration(std::uint64_t ns) {
  if (ns < 1'000) return std::format("{}ns", ns);
  if (ns < 1'000'000) return std::format("{}µs", ns / 1'000);
  if (ns < 1'000'000'000) return std::format("{:.2f}ms", static_cast<double>(ns) / 1e6);
  return std::format("{:.2f}s", static_cast<double>(ns) / 1e9);
}
// `812B`, `41.0KiB`, `1.24MiB`, `3.40GiB`; below zero (a frame that freed what others allocated)
// with a sign.
std::string format_bytes(std::int64_t b) {
  const std::string_view sign = b < 0 ? "-" : "";
  const auto n = static_cast<double>(b < 0 ? -b : b);
  if (n < 1024) return std::format("{}{}B", sign, b < 0 ? -b : b);
  if (n < 1024.0 * 1024) return std::format("{}{:.1f}KiB", sign, n / 1024);
  if (n < 1024.0 * 1024 * 1024) return std::format("{}{:.2f}MiB", sign, n / (1024.0 * 1024));
  return std::format("{}{:.2f}GiB", sign, n / (1024.0 * 1024 * 1024));
}

std::string format_worker(std::int32_t w) { return w < 0 ? "main" : std::format("w{}", w); }
// `main`, `3` or `w3`.
std::optional<std::int32_t> worker_named(std::string_view s) {
  if (s == "main") return -1;
  if (s.starts_with('w')) s.remove_prefix(1);
  std::int32_t n = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc{} || p != s.data() + s.size() || n < 0) return std::nullopt;
  return n;
}
std::string format_workers(const Event& e) {
  if (e.worker == e.end_worker) return format_worker(e.worker);
  return std::format("{}→{}", format_worker(e.worker), format_worker(e.end_worker));
}

// How a line is drawn: colors (a TTY without `NO_COLOR`), and times and workers, which a golden
// test leaves out because they differ between runs.
// Times, workers and memory are measurements of one run on one host: `times` off leaves them all
// out (a golden test).
struct Style {
  bool color = false;
  bool times = true;
  bool json = false;  // JSON Lines instead of text (`--trace-engine=json`)
};

std::string sgr(const Style& s, std::string_view params, std::string_view text) {
  if (!s.color) return std::string(text);
  return std::format("\x1b[{}m{}\x1b[0m", params, text);
}

// `Kind(key)`: the cell, as both modes name it.
std::string cell_name(const Event& e) { return e.key.empty() ? e.kind : std::format("{}({})", e.kind, e.key); }

// Memory (#274). An outcome that settles a cell for the request (it ran, was checked, kept by
// durability, or first read): where the bytes the cell retains count in sums. A reuse elsewhere
// is the same bytes, and counts nothing.
constexpr bool settles(Outcome o) {
  return o == Outcome::executed || o == Outcome::cutoff || o == Outcome::verified || o == Outcome::durable || o == Outcome::input;
}
// What a cell retains as sums count it: once, where it settles, never below zero.
std::int64_t counted_retained(const Event& e) { return settles(e.outcome) && !e.running ? std::max<std::int64_t>(0, e.retained_bytes) : 0; }
// Whether the event ran its cell's query (executed, or cut off early); a running one has not yet.
constexpr bool ran(const Event& e) { return !e.running && (e.outcome == Outcome::executed || e.outcome == Outcome::cutoff); }

// Sums over an event's subtree, itself included: what its frames allocated (only frames that ran
// allocate), and what the cells it settled retain.
struct Rollup {
  std::uint64_t alloc = 0;
  std::int64_t retained = 0;
};

// Each event's `Rollup`, for a pre-order batch with depths (the collector's).
std::vector<Rollup> rollups(std::span<const Event> events) {
  std::vector<Rollup> out(events.size());
  std::vector<std::optional<std::size_t>> parent(events.size());
  std::vector<std::size_t> open;  // the last event at each depth, on the path to here
  for (std::size_t i = 0; i < events.size(); ++i) {
    while (!open.empty() && events[open.back()].depth >= events[i].depth) open.pop_back();
    if (!open.empty() && events[open.back()].depth + 1 == events[i].depth) parent[i] = open.back();
    open.push_back(i);
  }
  for (std::size_t i = events.size(); i-- > 0;) {
    out[i].alloc += events[i].alloc_bytes;
    out[i].retained += counted_retained(events[i]);
    if (parent[i]) {
      out[*parent[i]].alloc += out[i].alloc;
      out[*parent[i]].retained += out[i].retained;
    }
  }
  return out;
}

// Sums per kind (#274): which stages use the memory. `peak` is the highest of its frames'.
struct KindSum {
  std::string kind;
  std::uint64_t executions = 0;
  std::uint64_t alloc_bytes = 0;
  std::uint64_t allocs = 0;
  std::int64_t peak_bytes = 0;
  std::int64_t retained_bytes = 0;
};
class Kinds {
 public:
  void add(const Event& e) {
    if (e.alloc_bytes == 0 && counted_retained(e) == 0) return;
    KindSum& k = by_name_[e.kind];
    k.kind = e.kind;
    k.executions += e.outcome == Outcome::executed || e.outcome == Outcome::cutoff;
    k.alloc_bytes += e.alloc_bytes;
    k.allocs += e.allocs;
    k.peak_bytes = std::max(k.peak_bytes, e.peak_bytes);
    k.retained_bytes += counted_retained(e);
  }
  // Most bytes allocated first, then most retained, then by name.
  std::vector<KindSum> sorted() const {
    std::vector<KindSum> out;
    for (const auto& [_, k] : by_name_) out.push_back(k);
    std::ranges::stable_sort(out, [](const KindSum& a, const KindSum& b) {
      return std::tuple(a.alloc_bytes, a.retained_bytes) > std::tuple(b.alloc_bytes, b.retained_bytes);
    });
    return out;
  }
  bool empty() const noexcept { return by_name_.empty(); }

 private:
  std::map<std::string, KindSum> by_name_;
};

// ` alloc 1.20MiB, 3.40MiB in all, retains 640.0KiB`: an event's own allocations, its subtree's
// when they are more, and what it retains where it settles.
std::string memory_text(const Event& e, const Rollup& r) {
  std::string out;
  if (e.alloc_bytes > 0) out += " alloc " + format_bytes(static_cast<std::int64_t>(e.alloc_bytes));
  if (r.alloc > e.alloc_bytes) out += std::format("{}{} in all", out.empty() ? " alloc " : ", ", format_bytes(static_cast<std::int64_t>(r.alloc)));
  if (counted_retained(e) > 0) out += std::format("{}retains {}", out.empty() ? " " : ", ", format_bytes(e.retained_bytes));
  return out;
}

// The stream's line: glyph, indentation by depth, the cell, its outcome, and its time, worker and
// memory.
//   ●   Tu(a.cpp) executed 1.24ms w2
std::string_view event_sgr(const Event& e) { return !e.errors.empty() ? error_sgr : e.running ? running_sgr : outcome_sgr(e.outcome); }

// `here`: the errors start at this cell (none of its children has any), so the whole line is
// marked, and its messages follow on their own lines.
std::string stream_line(const Event& e, const Style& s, bool here = false, std::string_view below = {}, const Rollup& r = {}) {
  std::string out = sgr(s, event_sgr(e), state_glyph(e, s.color));
  out += ' ';
  out.append(2 * e.depth, ' ');
  std::string body = sgr(s, here ? "" : "1", e.kind);
  if (!e.key.empty()) body += std::format("({})", e.key);
  body += ' ';
  body += here ? std::string(state_name(e)) : sgr(s, event_sgr(e), state_name(e));
  if (s.times) {
    const bool instant = e.end_ns == e.start_ns;
    const std::string t = (instant ? std::format(" {}", format_workers(e)) : std::format(" {} {}", format_duration(e.end_ns - e.start_ns), format_workers(e))) +
                          memory_text(e, r);
    body += here ? t : sgr(s, "2", t);
  }
  if (e.repeat > 1) body += here ? std::format(" ×{}", e.repeat) : sgr(s, "2", std::format(" ×{}", e.repeat));
  if (!e.errors.empty()) body += here ? " ← error here" : std::format(" (error below: {})", below);
  out += here ? sgr(s, error_here_sgr, body) : body;
  if (here) {
    for (auto line : e.errors | std::views::split('\n')) {
      if (line.empty()) continue;
      out += '\n';
      out.append(2 * e.depth + 4, ' ');
      out += sgr(s, error_sgr, std::format("error: {}", std::string_view(line.begin(), line.end())));
    }
  }
  return out;
}

// The first line of an error text.
std::string_view first_line(std::string_view s) { return s.substr(0, s.find('\n')); }

// Where errors start in a pre-order batch: `here` for an event with errors of its own and none
// below it; for one with errors below, `at` is the first such origin under it, so its message
// bubbles up to every ancestor.
struct Origins {
  std::vector<bool> here;
  std::vector<std::optional<std::size_t>> at;
};
// An outcome that computed the value (or checked it): where an error can be said to start. A
// reuse of the same cell elsewhere is red, but not a second origin.
constexpr bool computed(Outcome o) { return o == Outcome::executed || o == Outcome::cutoff || o == Outcome::verified; }

Origins error_origins(std::span<const Event> events) {
  Origins o{std::vector<bool>(events.size(), false), std::vector<std::optional<std::size_t>>(events.size())};
  std::set<std::uint64_t> computed_cells;
  for (const Event& e : events) {
    if (computed(e.outcome) && !e.errors.empty()) computed_cells.insert(e.cell);
  }
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i].errors.empty()) continue;
    bool below = false;
    for (std::size_t j = i + 1; j < events.size() && events[j].depth > events[i].depth; ++j) below = below || !events[j].errors.empty();
    // A reused cell is an origin only when no event here computed it (a rebuild that reused all).
    o.here[i] = !below && (computed(events[i].outcome) || !computed_cells.contains(events[i].cell));
  }
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i].errors.empty() || o.here[i]) continue;
    for (std::size_t j = i + 1; j < events.size() && events[j].depth > events[i].depth; ++j) {
      if (o.here[j]) {
        o.at[i] = j;
        break;
      }
    }
  }
  return o;
}

// A JSON string literal: quotes, backslashes and control characters escaped; UTF-8 as it is.
std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) std::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
        else out += c;
    }
  }
  return out + "\"";
}

// One event as a JSON object on one line (`--trace-engine=json`, JSON Lines): the tree is in
// `id`/`parent`, the depth is given, and `value` is there when the kind showed one.
std::string json_line(const Event& e, bool here = false, std::optional<std::uint64_t> at = std::nullopt, std::string_view request = {},
                      const Rollup& r = {}) {
  std::string out = std::format(R"({{"id":{},"parent":{},"depth":{},"kind":{},"key":{},"outcome":"{}","start_ns":{},"duration_ns":{},"worker":{},"end_worker":{})",
                                e.id, e.parent, e.depth, json_string(e.kind), json_string(e.key), outcome_name(e.outcome), e.start_ns,
                                e.end_ns - std::min(e.end_ns, e.start_ns), e.worker, e.end_worker);
  out += std::format(R"(,"alloc_bytes":{},"allocs":{},"peak_bytes":{},"retained_bytes":{},"incl_alloc_bytes":{},"incl_retained_bytes":{})",
                     e.alloc_bytes, e.allocs, e.peak_bytes, e.retained_bytes, r.alloc, r.retained);
  if (!request.empty()) out += ",\"request\":" + json_string(request);
  if (!e.value.empty()) out += ",\"value\":" + json_string(e.value);
  if (!e.errors.empty()) out += std::format(R"(,"errors":{},"error_here":{})", json_string(e.errors), here);
  if (at) out += std::format(R"(,"error_at":{})", *at);
  if (e.repeat > 1) out += std::format(R"(,"repeat":{})", e.repeat);
  if (!e.span.file.empty())
    out += std::format(R"(,"span":{{"file":{},"line":{},"column":{},"end_line":{},"end_column":{}}})", json_string(e.span.file), e.span.line,
                       e.span.column, e.span.end_line, e.span.end_column);
  return out + "}";
}

// Counts per outcome and the span of the trace.
struct Summary {
  std::array<std::uint64_t, outcome_count> counts{};
  std::uint64_t events = 0;
  std::uint64_t span_ns = 0;  // from the trace's start to its last event's end
  std::int32_t workers = 0;   // pool workers seen
  std::uint64_t running = 0;  // cells started and not ended yet (a live trace, #939); not in `events`
  // Memory (#274): what the frames allocated, and what the cells settled retain.
  std::uint64_t alloc_bytes = 0;
  std::uint64_t allocs = 0;
  std::int64_t retained_bytes = 0;
  void add_memory(const Event& e) {
    alloc_bytes += e.alloc_bytes;
    allocs += e.allocs;
    retained_bytes += counted_retained(e);
  }
};

// Histograms (#637): what groups of cells cost, by query kind, by source file, or by the children
// of a cell. Each metric sums over a group's cells, except `peak`, the highest of them.
enum class Metric : std::uint8_t { self_time, total_time, alloc, peak, retained, allocs, executions, hits, waits };
inline constexpr std::size_t metric_count = 9;
constexpr std::string_view metric_name(Metric m) {
  switch (m) {
    case Metric::self_time: return "self time";
    case Metric::total_time: return "time in all";
    case Metric::alloc: return "allocated";
    case Metric::peak: return "largest peak";
    case Metric::retained: return "retained";
    case Metric::allocs: return "allocations";
    case Metric::executions: return "runs";
    case Metric::hits: return "cache hits";
    case Metric::waits: return "waits";
  }
  return "?";
}
// The JSON field of each.
constexpr std::string_view metric_field(Metric m) {
  switch (m) {
    case Metric::self_time: return "self_ns";
    case Metric::total_time: return "total_ns";
    case Metric::alloc: return "alloc_bytes";
    case Metric::peak: return "peak_bytes";
    case Metric::retained: return "retained_bytes";
    case Metric::allocs: return "allocs";
    case Metric::executions: return "executions";
    case Metric::hits: return "hits";
    case Metric::waits: return "waits";
  }
  return "?";
}
std::string format_metric(Metric m, std::uint64_t v) {
  switch (m) {
    case Metric::self_time:
    case Metric::total_time: return format_duration(v);
    case Metric::alloc:
    case Metric::peak:
    case Metric::retained: return format_bytes(static_cast<std::int64_t>(v));
    default: return std::to_string(v);
  }
}
// One cell's share of each metric. Self time is its own span less the time its children's spans
// cover inside it (they may run in parallel: their union counts). Time in all is its span, counted
// only for the outermost cell of a group on each demand path (`outermost`), so a group nested in
// itself counts its time once. A wait's span is waiting, not work: no time. A cache hit is a cell
// found green (reused, verified, kept by durability), counted as often as the cell was read
// (`repeat`); a wait, a cell another frame owned (waited for, or computed privately).
std::array<std::uint64_t, metric_count> metrics_of(const Event& e, std::uint64_t self_ns, bool outermost = true) {
  std::array<std::uint64_t, metric_count> v{};
  const auto at = [&](Metric m) -> std::uint64_t& { return v[std::to_underlying(m)]; };
  const bool waiting = e.outcome == Outcome::waited && !e.running;
  at(Metric::self_time) = waiting ? 0 : self_ns;
  at(Metric::total_time) = waiting || !outermost ? 0 : e.end_ns - std::min(e.end_ns, e.start_ns);
  if (e.running) return v;  // a running cell (#939): its time so far; its outcome and memory come at its end
  at(Metric::alloc) = e.alloc_bytes;
  at(Metric::peak) = static_cast<std::uint64_t>(std::max<std::int64_t>(0, e.peak_bytes));
  at(Metric::retained) = static_cast<std::uint64_t>(counted_retained(e));
  at(Metric::allocs) = e.allocs;
  at(Metric::executions) = e.outcome == Outcome::executed || e.outcome == Outcome::cutoff;
  at(Metric::hits) = e.outcome == Outcome::reused || e.outcome == Outcome::verified || e.outcome == Outcome::durable ? e.repeat : 0;
  at(Metric::waits) = e.outcome == Outcome::waited || e.outcome == Outcome::recomputed;
  return v;
}
// [from, to) less the union of `inner` intervals, clipped to it.
std::uint64_t uncovered(std::uint64_t from, std::uint64_t to, std::vector<std::pair<std::uint64_t, std::uint64_t>> inner) {
  if (to <= from) return 0;
  std::ranges::sort(inner);
  std::uint64_t covered = 0, reach = from;
  for (auto [a, b] : inner) {
    a = std::clamp(a, reach, to);
    b = std::clamp(b, a, to);
    covered += b - a;
    reach = std::max(reach, b);
  }
  return to - from - covered;
}
// Each event's parent in a batch, by its `parent` id (a late child, handed on after its parent's
// batch, has none here; the collector's `finish` may put one anywhere).
std::vector<std::optional<std::size_t>> parents_in(std::span<const Event> events) {
  std::unordered_map<std::uint64_t, std::size_t> at;
  for (std::size_t i = 0; i < events.size(); ++i) at.emplace(events[i].id, i);
  std::vector<std::optional<std::size_t>> out(events.size());
  for (std::size_t i = 0; i < events.size(); ++i)
    if (const auto it = at.find(events[i].parent); events[i].parent != 0 && it != at.end() && it->second != i) out[i] = it->second;
  return out;
}
// Each event's self time, for a batch (the collector's).
std::vector<std::uint64_t> self_times(std::span<const Event> events) {
  std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> kids(events.size());
  const auto parents = parents_in(events);
  for (std::size_t i = 0; i < events.size(); ++i)
    if (parents[i]) kids[*parents[i]].emplace_back(events[i].start_ns, events[i].end_ns);
  std::vector<std::uint64_t> out(events.size());
  for (std::size_t i = 0; i < events.size(); ++i) out[i] = uncovered(events[i].start_ns, events[i].end_ns, std::move(kids[i]));
  return out;
}

struct Group {
  std::string name;
  std::uint64_t cells = 0;  // cells read, counting each repeat
  std::array<std::uint64_t, metric_count> v{};
  std::int32_t node = -1;  // by children: the child
  std::uint64_t at(Metric m) const { return v[std::to_underlying(m)]; }
  void add(const std::array<std::uint64_t, metric_count>& x, std::uint32_t repeat = 1) {
    cells += repeat;
    for (std::size_t i = 0; i < metric_count; ++i) v[i] = i == std::to_underlying(Metric::peak) ? std::max(v[i], x[i]) : v[i] + x[i];
  }
};
// Groups by key (a name, or a node by children), most of `by` first, then by key.
class Groups {
 public:
  Group& add(const std::string& key, const std::string& name, const std::array<std::uint64_t, metric_count>& x, std::uint32_t repeat = 1) {
    Group& g = by_name_[key];
    g.name = name;
    g.add(x, repeat);
    return g;
  }
  std::vector<Group> sorted(Metric by) const {
    std::vector<Group> out;
    for (const auto& [_, g] : by_name_) out.push_back(g);
    std::ranges::stable_sort(out, std::greater{}, [&](const Group& g) { return g.at(by); });
    return out;
  }
  bool empty() const noexcept { return by_name_.empty(); }

 private:
  std::map<std::string, Group> by_name_;
};
// A file's group: its span's file; `(no source)` for a cell with none.
std::string_view file_group_name(const Event& e) { return e.span.file.empty() ? std::string_view("(no source)") : std::string_view(e.span.file); }
std::string file_group(const Event& e) { return std::string(file_group_name(e)); }
// A filter's words: split at spaces outside double quotes, the quotes dropped (`at:"/my dir/a.cpp"`).
std::vector<std::string> filter_words(std::string_view text) {
  std::vector<std::string> out;
  std::string word;
  bool quoted = false;
  for (const char c : text) {
    if (c == '"') quoted = !quoted;
    else if (c == ' ' && !quoted) {
      if (!word.empty()) out.push_back(std::move(word));
      word.clear();
    } else word += c;
  }
  if (!word.empty()) out.push_back(std::move(word));
  return out;
}
// A word as a filter's text: quoted when it has a space.
std::string filter_text(const std::vector<std::string>& words) {
  std::string out;
  for (const std::string& w : words) {
    if (!out.empty()) out += ' ';
    if (!w.contains(' ')) {
      out += w;
    } else if (const std::size_t colon = w.find(':'); colon != std::string::npos) {
      out += w.substr(0, colon + 1) + '"' + w.substr(colon + 1) + '"';
    } else {
      out += '"' + w + '"';
    }
  }
  return out;
}
// `[{"kind":"Tu","cells":3,"self_ns":…,…},…]`: groups for JSON, under `key`.
std::string groups_json(const std::vector<Group>& gs, std::string_view key) {
  std::string out = "[";
  for (const Group& g : gs) {
    std::format_to(std::back_inserter(out), R"({}{{"{}":{},"cells":{})", out.size() > 1 ? "," : "", key, json_string(g.name), g.cells);
    for (std::size_t i = 0; i < metric_count; ++i) std::format_to(std::back_inserter(out), R"(,"{}":{})", metric_field(static_cast<Metric>(i)), g.v[i]);
    out += '}';
  }
  return out + "]";
}

// The browser's tree. Events arrive as collector batches (`add`: whole subtrees in pre-order) or
// live (`apply`, #939): each frame's start and end, and each instant, as they happen, in any
// order. Either way a node's children are in id order, which is their start order, and identical
// instants in a row under one parent are folded into the first (`repeat`), as the collector folds
// them, so the tree a live trace ends with is the one its batches build, whatever order the events
// came in.
class Tree {
 public:
  struct Node {
    Event ev;
    std::int32_t parent = -1;  // index, -1 at the top
    std::vector<std::int32_t> children;  // in id order; folded instants left out
    bool expanded = false;
    bool group = false;  // a request of a watched daemon, holding its top-level events
    // In this node's subtree (itself included): events, and executions (executed or cutoff).
    std::uint64_t subtree = 1;
    std::uint64_t executions = 0;
    Rollup incl;  // memory in its subtree, itself included (#274)
    // Live (#939). Whether it is in the tree: false while its parent has not come.
    bool attached = true;
    // An instant folded into an earlier sibling, `folded_into`, which counts it in its `repeat`;
    // it is shown, counted and summed nowhere.
    std::int32_t folded_into = -1;
    std::vector<std::int32_t> members;  // the instants folded into this one, in id order
    std::uint32_t own_repeat = 1;       // `ev.repeat` as it came, before folding added to it
  };
  // How siblings are ordered: as they started, or most memory first (#274): bytes allocated in
  // the subtree, or retained in it; or longest first (#635).
  enum class Order : std::uint8_t { start, alloc, retained, time };
  static constexpr std::uint8_t order_count = 4;
  struct Row {
    std::int32_t node;
    std::uint32_t depth;
    bool match;  // matches the filter itself (else shown as an ancestor of a match)
    bool operator==(const Row&) const = default;
  };

  // Hides reused cells (leaves only: a request's node stays), to see what ran.
  void set_hide_reused(bool on) noexcept { hide_reused_ = on, changed(); }
  bool hide_reused() const noexcept { return hide_reused_; }

  // Top-level nodes newest first (the live views' default: the latest request on top).
  void set_newest_first(bool on) noexcept { newest_first_ = on, changed(); }
  bool newest_first() const noexcept { return newest_first_; }

  void set_order(Order o) noexcept { order_ = o, changed(); }
  Order order() const noexcept { return order_; }

  // The latest build's totals beyond its events, for the header (#274); while a live trace runs,
  // the process's bytes now and their peak so far.
  void set_memory(BuildMemory m) noexcept { memory_ = m; }
  std::optional<BuildMemory> memory() const noexcept { return memory_; }

  // A request's node (`ctl trace`, `--watch`), under which `add(batch, id)` puts its top-level
  // events. It opens folded the earlier requests', so the latest is the one shown whole.
  void open_group(std::uint64_t id, std::string label, std::uint64_t start_ns) {
    for (std::int32_t r : roots_) {
      if (nodes_[r].group) nodes_[r].expanded = false;
    }
    Node n;
    n.ev.id = id;
    n.ev.kind = "request";
    n.ev.key = std::move(label);
    n.ev.start_ns = n.ev.end_ns = start_ns;
    n.ev.outcome = Outcome::verified;
    n.group = true;
    n.executions = 0;
    n.expanded = true;
    const auto at = static_cast<std::int32_t>(nodes_.size());
    by_id_.emplace(id, at);
    nodes_.push_back(std::move(n));
    insert_sorted(roots_, at);
    changed();
  }
  // Text for a request's node (`--watch`: its diagnostics), shown in the detail pane.
  void annotate(std::uint64_t id, std::string text, bool append = false) {
    if (const auto it = by_id_.find(id); it != by_id_.end()) {
      std::string& v = nodes_[it->second].ev.value;
      v = append ? v + text : std::move(text);
    }
  }
  // A request's end: its span, and its outcome: executed if anything under it ran. Live, the
  // events whose parent never came go under it (`settle`).
  void close_group(std::uint64_t id, std::uint64_t end_ns) {
    settle(id);
    if (const auto it = by_id_.find(id); it != by_id_.end()) {
      Node& n = nodes_[it->second];
      n.ev.end_ns = std::max(n.ev.start_ns, end_ns);
      n.ev.outcome = n.executions > 0 ? Outcome::executed : Outcome::reused;
      // Red too when its build's values hold errors (they are shown below it).
      if (std::ranges::any_of(n.children, [&](std::int32_t c) { return !nodes_[c].ev.errors.empty(); }))
        n.ev.errors = "the build has errors";
    }
    changed();
  }

  // Events in collector order; with `group`, top-level ones go under that request's node, and so
  // does an event whose parent is not here (a late child of an earlier batch's).
  // A batch comes folded already: it is placed as it is.
  void add(std::span<const Event> batch, std::uint64_t group = 0) {
    folding_ = false;
    for (const Event& e : batch) place(e, group, false);
    folding_ = true;
    changed();
  }
  // Live events (#939), in the order they came, which need not be the order they happened in: a
  // frame's start (`running`), its end, or an instant. A start makes a running node and its end
  // settles it; an end with no start before it (an instant, or a start that has not come yet) is
  // placed settled. An event whose parent has not come waits for it, unseen, until `settle`.
  void apply(std::span<const Event> events, std::uint64_t group = 0) {
    for (const Event& e : events) place(e, group, true);
    changed();
  }
  // The end of a live request (`group`; 0: the whole trace): what still waits for its parent goes
  // under the request's node, or to the top, as the collector hands orphans on at its end.
  void settle(std::uint64_t group = 0) {
    std::vector<std::pair<std::uint64_t, std::int32_t>> due;  // (id, node), placed in id order
    for (auto it = waiting_.begin(); it != waiting_.end();) {
      std::erase_if(it->second, [&](const Waiting& w) {
        if (w.group != group) return false;
        due.emplace_back(nodes_[w.node].ev.id, w.node);
        return true;
      });
      it = it->second.empty() ? waiting_.erase(it) : std::next(it);
    }
    std::ranges::sort(due);
    for (const auto& [_, i] : due) {
      // Still unattached: an earlier one in `due` may have been its parent, and taken it along.
      if (!nodes_[i].attached) {
        nodes_[i].ev.parent = group;
        attach(i, group_node(group));
      }
    }
    // What still runs never ends (a run abandoned for another: a lost daemon): it failed, where
    // its time reached.
    const std::int32_t g = group_node(group);
    for (std::int32_t i : std::vector(running_)) {
      if (!nodes_[i].ev.running) continue;
      std::int32_t top = i;
      while (nodes_[top].parent >= 0 && nodes_[top].parent != g) top = nodes_[top].parent;
      if (group != 0 && nodes_[top].parent != g) continue;  // another request's
      Event e = nodes_[i].ev;
      e.running = false;
      e.outcome = Outcome::failed;
      settle_node(i, std::move(e));
    }
    changed();
  }
  // Everything shown goes (a new run of the request: a retry, the fallback in-process), the way it
  // is shown stays; `resets` counts them, so a view holding nodes knows they are gone.
  void reset() {
    Tree fresh;
    fresh.newest_first_ = newest_first_;
    fresh.hide_reused_ = hide_reused_;
    fresh.order_ = order_;
    fresh.words_ = words_;
    fresh.raw_words_ = raw_words_;
    fresh.worker_ = worker_;
    fresh.resets_ = resets_ + 1;
    fresh.version_ = version_ + 1;
    *this = std::move(fresh);
  }
  std::uint64_t resets() const noexcept { return resets_; }
  // Moves running nodes' ends to `now_ns` (since the trace began), so their times, the timeline
  // and the span grow while they run (#939).
  void advance(std::uint64_t now_ns) {
    std::erase_if(running_, [&](std::int32_t i) { return !nodes_[i].ev.running; });
    if (running_.empty()) return;
    for (std::int32_t i : running_) nodes_[i].ev.end_ns = std::max(nodes_[i].ev.start_ns, now_ns);
    if (order_ == Order::time) changed();
  }
  // Cells started and not ended yet.
  std::uint64_t running() const noexcept { return summary_.running; }
  // The newest activity (#939): the cell started last that still runs, else the one that came last.
  std::optional<std::int32_t> newest() const {
    std::optional<std::int32_t> best;
    for (std::int32_t i : running_) {
      const Node& n = nodes_[i];
      if (n.ev.running && n.attached && (!best || n.ev.id > nodes_[*best].ev.id)) best = i;
    }
    if (!best && last_ >= 0) best = last_;
    return best;
  }
  // The node to show for `i`: itself, or the one it is folded into.
  std::int32_t shown(std::int32_t i) const {
    contract_assert(i >= 0 && static_cast<std::size_t>(i) < nodes_.size());  // a node of this tree, not of one reset since
    return nodes_[i].folded_into >= 0 ? nodes_[i].folded_into : i;
  }

  std::span<const Node> nodes() const noexcept { return nodes_; }
  const Node& node(std::int32_t i) const { return nodes_[i]; }
  std::span<const std::int32_t> roots() const noexcept { return roots_; }
  // The tree's nodes in pre-order, children in id order (folded instants and nodes still waiting
  // for their parent left out).
  std::vector<std::int32_t> preorder() const {
    std::vector<std::int32_t> out;
    const auto walk = [&](this const auto& self, std::int32_t i) -> void {
      out.push_back(i);
      for (std::int32_t c : nodes_[i].children) self(c);
    };
    for (std::int32_t r : roots_) walk(r);
    return out;
  }
  // Changes with anything that can change what is shown; equal values: nothing has.
  std::uint64_t version() const noexcept { return version_; }

  // The first node under `i` where errors start (pre-order), if any: what bubbles up to `i`.
  std::optional<std::int32_t> error_origin_below(std::int32_t i) const {
    for (std::int32_t c : nodes_[i].children) {
      if (error_here(c)) return c;
      if (auto deeper = error_origin_below(c)) return deeper;
    }
    return std::nullopt;
  }

  // Errors start at node `i`: its value holds some, nothing below it does, and it computed the
  // value (a reuse is an origin only when no node computed that cell), as the stream decides.
  bool error_here(std::int32_t i) const {
    const Node& n = nodes_[i];
    if (n.ev.errors.empty() || errors_below(i)) return false;
    return computed(n.ev.outcome) || !computed_with_errors_.contains(n.ev.cell);
  }

  void toggle(std::int32_t i) {
    if (!nodes_[i].children.empty()) nodes_[i].expanded = !nodes_[i].expanded, changed();
  }
  void set_expanded(std::int32_t i, bool open) { nodes_[i].expanded = open && !nodes_[i].children.empty(), changed(); }
  void expand_all(bool open) {
    for (Node& n : nodes_) n.expanded = open && !n.children.empty();
    changed();
  }
  // Opens every ancestor of `i`, so it is visible.
  void reveal(std::int32_t i) {
    for (std::int32_t p = nodes_[i].parent; p >= 0; p = nodes_[p].parent) nodes_[p].expanded = true;
    changed();
  }

  // The filter: words that must all match. `word` matches the kind or key (case-insensitively),
  // `is:executed` (or a prefix, `is:ex`) an outcome, `is:running` the cells still running, `w:3`
  // or `w:main` a worker, `at:a.cpp:12` the cells whose source span touches that line (or file).
  // Empty: all.
  // Words split at spaces outside double quotes, the quotes dropped: `at:"/my dir/a.cpp"`.
  void set_filter(std::string_view text) {
    raw_words_ = filter_words(text);
    words_.clear();
    for (const std::string& w : raw_words_) words_.push_back(lower(w));
    changed();
  }
  // Only events a worker ran (`std::nullopt`: any), on top of the text filter.
  void set_worker(std::optional<std::int32_t> w) { worker_ = w, changed(); }
  std::optional<std::int32_t> worker() const noexcept { return worker_; }
  bool filtering() const noexcept { return !words_.empty() || worker_.has_value(); }

  bool matches(const Event& e) const {
    if (worker_ && e.worker != *worker_ && e.end_worker != *worker_) return false;
    if (words_.empty()) return true;
    const std::string kind = lower(e.kind), key = lower(e.key);
    for (std::size_t i = 0; i < words_.size(); ++i) {
      const std::string& w = words_[i];
      if (w.starts_with("is:")) {
        const std::string_view name = std::string_view(w).substr(3);
        // An outcome by prefix first, as ever (`is:r` is reused); then the running cells.
        if (const auto o = outcome_named(name)) {
          if (e.running || *o != e.outcome) return false;
        } else if (name.empty() || !std::string_view("running").starts_with(name) || !e.running) {
          return false;
        }
      } else if (w.starts_with("w:")) {
        const std::optional<std::int32_t> n = worker_named(std::string_view(w).substr(2));
        if (!n || (e.worker != *n && e.end_worker != *n)) return false;
      } else if (w.starts_with("kind:")) {
        if (kind != std::string_view(w).substr(5)) return false;  // a whole kind (a histogram's bar)
      } else if (w.starts_with("at:")) {
        // Paths keep their case: the word as typed, not lowered.
        if (!span_touches(e.span, std::string_view(raw_words_[i]).substr(3))) return false;
      } else if (!kind.contains(w) && !key.contains(w)) {
        return false;
      }
    }
    return true;
  }

  // The rows to draw. Unfiltered: what expansion shows. Filtered: every match, under its
  // ancestors (which are shown whether expanded or not), and a match's own subtree as expanded.
  // Kept until something changes (`version`): a live view asks for them many times a frame.
  std::vector<Row> rows() const {
    if (rows_at_ == version_ && rows_valid_) return rows_;
    std::vector<Row> out;
    if (!filtering()) {
      for (std::int32_t r : ordered_roots()) walk(r, 0, out);
    } else {
      std::vector<char> keep(nodes_.size(), 0);  // 1: an ancestor of a match, 2: a match
      for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i].attached || nodes_[i].folded_into >= 0 || !matches(nodes_[i].ev)) continue;
        keep[i] = 2;
        for (std::int32_t p = nodes_[i].parent; p >= 0 && !keep[p]; p = nodes_[p].parent) keep[p] = 1;
      }
      for (std::int32_t r : ordered_roots()) walk_filtered(r, 0, keep, out);
    }
    rows_ = out;
    rows_at_ = version_;
    rows_valid_ = true;
    return out;
  }

  // Counts per outcome, the span, the workers and the memory, kept as events come.
  // A running cell counts its start in the span kept as nodes come, and its time so far here, so
  // that moving it on (`advance`) or ending it never makes the summary count again.
  Summary summary() const {
    if (summary_dirty_) recount();
    Summary s = summary_;
    for (std::int32_t i : running_)
      if (nodes_[i].ev.running) s.span_ns = std::max(s.span_ns, nodes_[i].ev.end_ns);
    return s;
  }

  static constexpr std::uint32_t open_depth = 2;
  static constexpr std::int32_t max_worker = 1023;

 private:
  struct Waiting {
    std::int32_t node;
    std::uint64_t group;
  };

  void changed() noexcept { ++version_; }
  static std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
  }
  // `ids` (node indices) kept in event id order: `i` goes after every smaller id; true when it went
  // last.
  bool insert_sorted(std::vector<std::int32_t>& ids, std::int32_t i) const {
    const std::uint64_t id = nodes_[i].ev.id;
    const auto at = std::ranges::upper_bound(ids, id, {}, [&](std::int32_t k) { return nodes_[k].ev.id; });
    const bool last = at == ids.end();
    ids.insert(at, i);
    return last;
  }
  std::int32_t group_node(std::uint64_t group) const {
    if (group == 0) return -1;
    const auto it = by_id_.find(group);
    return it == by_id_.end() ? -1 : it->second;
  }

  // One event, from a batch (`wait` false: an unknown parent means the request's node) or live.
  void place(const Event& given, std::uint64_t group, bool wait) {
    Event e = given;
    // From the wire: an outcome and workers in range (lanes are indexed by worker).
    if (std::to_underlying(e.outcome) >= outcome_count) e.outcome = Outcome::failed;
    e.worker = std::clamp(e.worker, -1, max_worker);
    e.end_worker = std::clamp(e.end_worker, -1, max_worker);
    e.end_ns = std::max(e.end_ns, e.start_ns);
    if (e.repeat == 0) e.repeat = 1;
    if (const auto it = by_id_.find(e.id); it != by_id_.end()) {
      // The end of a frame whose start came; a second start or end of one event changes nothing.
      if (!e.running && nodes_[it->second].ev.running && !nodes_[it->second].group) settle_node(it->second, std::move(e));
      return;
    }
    const auto at = static_cast<std::int32_t>(nodes_.size());
    Node n;
    n.ev = std::move(e);
    n.own_repeat = n.ev.repeat;
    n.executions = ran(n.ev);
    n.incl = {n.ev.alloc_bytes, counted_retained(n.ev)};
    n.attached = false;
    const std::uint64_t parent = n.ev.parent;
    by_id_.emplace(n.ev.id, at);
    nodes_.push_back(std::move(n));
    last_ = at;
    count(at, +1);
    if (nodes_[at].ev.running) running_.push_back(at);
    else if (computed(nodes_[at].ev.outcome) && !nodes_[at].ev.errors.empty()) computed_with_errors_.insert(nodes_[at].ev.cell);
    const auto p = parent == 0 ? by_id_.end() : by_id_.find(parent);
    if (parent != 0 && p != by_id_.end() && p->second != at) {
      attach(at, p->second);
    } else if (parent != 0 && wait) {
      waiting_[parent].push_back({at, group});
    } else {
      nodes_[at].ev.parent = group;
      attach(at, group_node(group));
    }
    // Children that came before it.
    if (auto kids = waiting_.extract(nodes_[at].ev.id); !kids.empty()) {
      std::vector<std::int32_t> ks;
      for (const Waiting& w : kids.mapped()) ks.push_back(w.node);
      std::ranges::sort(ks, {}, [&](std::int32_t k) { return nodes_[k].ev.id; });
      for (std::int32_t k : ks) attach(k, at);
    }
  }

  // A running node's end (#939): what it ran to, its memory, value and errors.
  void settle_node(std::int32_t i, Event e) {
    Node& n = nodes_[i];
    count(i, -1);
    e.depth = n.ev.depth;
    e.parent = n.ev.parent;  // a top-level event's is its request's
    e.repeat = n.own_repeat;
    const std::uint64_t exec = ran(e);
    const Rollup own{e.alloc_bytes, counted_retained(e)};
    n.ev = std::move(e);
    count(i, +1);
    if (computed(n.ev.outcome) && !n.ev.errors.empty()) computed_with_errors_.insert(n.ev.cell);
    // Its own share, into its subtree's sums and up (it counted nothing while it ran).
    for (std::int32_t p = i; p >= 0; p = nodes_[p].parent) {
      nodes_[p].executions += exec;
      nodes_[p].incl.alloc += own.alloc;
      nodes_[p].incl.retained += own.retained;
      if (nodes_[p].folded_into >= 0) break;  // a folded node counts nowhere above it
    }
    // Ended at once, with nothing under it: an instant like its neighbours, perhaps.
    if (n.attached && instant_leaf(i) && foldable(n.parent)) refold_around(n.parent, i);
  }

  // Puts node `i` (and what came under it before it) under `p` (-1: the top).
  void attach(std::int32_t i, std::int32_t p) {
    // A parent that was an instant leaf is one no more: its siblings fold again around it.
    // While `p` is folded, the sums stop at it (below), and its unfolding counts it whole.
    const bool was_instant = p >= 0 && nodes_[p].attached && instant_leaf(p) && foldable(nodes_[p].parent);
    Node& n = nodes_[i];
    n.parent = p;
    n.attached = p < 0 || nodes_[p].attached;
    // A subtree that waited for its parent joins the tree: it folds now.
    const bool joins = n.attached && !n.children.empty();
    if (n.attached) set_depth(i, p < 0 || nodes_[p].group ? 0 : nodes_[p].ev.depth + 1);
    const bool last = insert_sorted(p < 0 ? roots_ : nodes_[p].children, i);
    // Its subtree's sums, up to the top (of the tree, or of a subtree still waiting for its
    // parent) or the first folded node, which counts nowhere above it.
    for (std::int32_t a = p; a >= 0; a = nodes_[a].parent) {
      nodes_[a].subtree += nodes_[i].subtree;
      nodes_[a].executions += nodes_[i].executions;
      nodes_[a].incl.alloc += nodes_[i].incl.alloc;
      nodes_[a].incl.retained += nodes_[i].incl.retained;
      if (nodes_[a].folded_into >= 0) break;  // a folded node counts nowhere above it
    }
    if (joins) refold_below(i);
    if (was_instant) refold_around(nodes_[p].parent, p);
    if (!foldable(p)) return;
    // Last among those shown, but perhaps not after what the one before it folded in.
    const auto& kids = nodes_[p].children;
    const bool inside = last && kids.size() > 1 && !nodes_[kids[kids.size() - 2]].members.empty() &&
                        nodes_[nodes_[kids[kids.size() - 2]].members.back()].ev.id > nodes_[i].ev.id;
    if (!last || inside) refold_around(p, i);
    else fold_last(p);
  }
  // Depths (as the collector gives them: 0 at the top and under a request's node) and the first
  // expansion, for a subtree that joins the tree.
  void set_depth(std::int32_t i, std::uint32_t depth) {
    Node& n = nodes_[i];
    n.ev.depth = depth;
    n.attached = true;
    // Top-level nodes and the first levels below them start open, the rest closed.
    n.expanded = depth < open_depth;
    for (std::int32_t m : n.members) nodes_[m].ev.depth = depth, nodes_[m].attached = true;
    for (std::int32_t c : n.children) set_depth(c, depth + 1);
  }

  // Folding: children of an event (not the top, not a request's node) fold as the collector folds
  // them: an instant with nothing under it, just like the instant before it (in id order, with
  // nothing else between), counts in that one's `repeat`.
  bool foldable(std::int32_t p) const { return folding_ && p >= 0 && !nodes_[p].group && nodes_[p].attached && nodes_[p].folded_into < 0; }
  bool instant_leaf(std::int32_t i) const {
    const Node& n = nodes_[i];
    return !n.ev.running && n.ev.start_ns == n.ev.end_ns && n.children.empty();
  }
  bool same_instant(std::int32_t a, std::int32_t b) const {
    const Event &x = nodes_[a].ev, &y = nodes_[b].ev;
    return x.cell == y.cell && x.kind == y.kind && x.outcome == y.outcome && x.errors == y.errors;
  }
  // The last child of `p` just came: it folds into the one before it if they are alike.
  void fold_last(std::int32_t p) {
    auto& kids = nodes_[p].children;
    if (kids.size() < 2) return;
    const std::int32_t c = kids.back(), prev = kids[kids.size() - 2];
    if (!instant_leaf(c) || !instant_leaf(prev) || !same_instant(prev, c)) return;
    kids.pop_back();
    absorb(c, prev);
  }
  // `c`, out of its parent's shown children, counts in `head`'s repeat from now on, and nowhere
  // else.
  void absorb(std::int32_t c, std::int32_t head) {
    hide(c, -1);
    nodes_[c].folded_into = head;
    insert_sorted(nodes_[head].members, c);
    nodes_[head].ev.repeat += nodes_[c].own_repeat;
  }
  // Where shown child `h` is among `p`'s (they are in id order).
  std::size_t position(std::int32_t p, std::int32_t h) const {
    const auto& kids = nodes_[p].children;
    return static_cast<std::size_t>(std::ranges::lower_bound(kids, nodes_[h].ev.id, {}, [&](std::int32_t k) { return nodes_[k].ev.id; }) - kids.begin());
  }
  // The instants folded into shown child `h` of `p` shown again, each where its id puts it (a
  // child that just came may be among them).
  void unfold_head(std::int32_t p, std::int32_t h) {
    Node& head = nodes_[h];
    if (head.members.empty()) return;
    auto& kids = nodes_[p].children;
    for (std::int32_t m : head.members) {
      nodes_[m].folded_into = -1;
      hide(m, +1);
      insert_sorted(kids, m);
    }
    head.members.clear();
    head.ev.repeat = head.own_repeat;
  }
  // `p`'s children folded afresh around child `c` (shown, or folded into one that is): after it
  // came out of order, or its shape changed. Only its run and its neighbours' can change: the runs
  // beyond them are cut off from it by shown children that stay as they are.
  void refold_around(std::int32_t p, std::int32_t c) {
    auto& kids = nodes_[p].children;
    const std::size_t k = position(p, shown(c));
    const std::size_t lo = k > 0 ? k - 1 : 0, hi = std::min(k + 1, kids.size() - 1);
    const std::int32_t after = hi + 1 < kids.size() ? kids[hi + 1] : -1;  // the first shown child past them
    for (std::size_t j = hi + 1; j-- > lo;) unfold_head(p, kids[j]);
    const std::size_t end = after < 0 ? kids.size() : position(p, after);
    std::int32_t last = lo > 0 && instant_leaf(kids[lo - 1]) ? kids[lo - 1] : -1;  // the previous child, if it can take a repeat
    std::vector<std::int32_t> out;
    for (std::size_t j = lo; j < end; ++j) {
      const std::int32_t x = kids[j];
      if (instant_leaf(x) && last >= 0 && same_instant(last, x)) {
        absorb(x, last);
        continue;
      }
      out.push_back(x);
      last = instant_leaf(x) ? x : -1;
    }
    kids.erase(kids.begin() + static_cast<std::ptrdiff_t>(lo), kids.begin() + static_cast<std::ptrdiff_t>(end));
    kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(lo), out.begin(), out.end());
  }
  // Every node of `i`'s subtree folded afresh, deepest first (a subtree that waited for its
  // parent, which folded nothing meanwhile).
  void refold_below(std::int32_t i) {
    for (std::int32_t c : std::vector(nodes_[i].children)) refold_below(c);
    if (!foldable(i) || nodes_[i].children.empty()) return;
    auto& kids = nodes_[i].children;
    std::vector<std::int32_t> out;
    std::int32_t last = -1;
    for (std::int32_t x : kids) {
      if (instant_leaf(x) && last >= 0 && same_instant(last, x)) {
        absorb(x, last);
        continue;
      }
      out.push_back(x);
      last = instant_leaf(x) ? x : -1;
    }
    kids = std::move(out);
  }
  // A folded instant's share taken out of (`sign` -1) or put back into (+1) its ancestors' sums
  // and the summary.
  void hide(std::int32_t c, int sign) {
    const Node& n = nodes_[c];
    for (std::int32_t a = n.parent; a >= 0; a = nodes_[a].parent) {
      Node& x = nodes_[a];
      x.subtree = static_cast<std::uint64_t>(static_cast<std::int64_t>(x.subtree) + sign * static_cast<std::int64_t>(n.subtree));
      x.executions = static_cast<std::uint64_t>(static_cast<std::int64_t>(x.executions) + sign * static_cast<std::int64_t>(n.executions));
      x.incl.alloc = static_cast<std::uint64_t>(static_cast<std::int64_t>(x.incl.alloc) + sign * static_cast<std::int64_t>(n.incl.alloc));
      x.incl.retained += sign * n.incl.retained;
      if (x.folded_into >= 0) break;
    }
    count(c, sign);
  }

  // The summary, kept as nodes come, settle, fold and unfold. Taking one out whose end or worker
  // may have been the highest leaves the span and workers to count again.
  void count(std::int32_t i, int sign) {
    const Node& n = nodes_[i];
    if (n.group) return;
    const Event& e = n.ev;
    Summary& s = summary_;
    const auto step = [&](auto& v) { v = sign > 0 ? v + 1 : v - 1; };
    if (e.running) {
      step(s.running);
    } else {
      step(s.counts[std::to_underlying(e.outcome)]);
      step(s.events);
      s.alloc_bytes = sign > 0 ? s.alloc_bytes + e.alloc_bytes : s.alloc_bytes - e.alloc_bytes;
      s.allocs = sign > 0 ? s.allocs + e.allocs : s.allocs - e.allocs;
      s.retained_bytes += sign * counted_retained(e);
    }
    const std::int32_t top = std::max(e.worker, e.end_worker);
    const std::uint64_t reach = e.running ? e.start_ns : e.end_ns;
    if (sign > 0) {
      s.span_ns = std::max(s.span_ns, reach);
      s.workers = std::max(s.workers, top + 1);
    } else if (reach >= s.span_ns || top + 1 >= s.workers) {
      summary_dirty_ = true;
    }
  }
  void recount() const {
    Summary s;
    std::int32_t top = -1;
    for (const Node& n : nodes_) {
      if (n.group || n.folded_into >= 0) continue;
      if (n.ev.running) ++s.running;
      else ++s.counts[std::to_underlying(n.ev.outcome)], ++s.events, s.add_memory(n.ev);
      s.span_ns = std::max(s.span_ns, n.ev.running ? n.ev.start_ns : n.ev.end_ns);
      top = std::max({top, n.ev.worker, n.ev.end_worker});
    }
    s.workers = top + 1;
    summary_ = s;
    summary_dirty_ = false;
  }

  bool errors_below(std::int32_t i) const {
    return std::ranges::any_of(nodes_[i].children, [&](std::int32_t c) { return !nodes_[c].ev.errors.empty() || errors_below(c); });
  }
  bool hidden(std::int32_t i) const {
    const Node& n = nodes_[i];
    return hide_reused_ && !n.group && n.children.empty() && !n.ev.running && n.ev.outcome == Outcome::reused;
  }
  // Siblings in the chosen order; ties keep their start order.
  std::vector<std::int32_t> ordered(std::span<const std::int32_t> ids) const {
    std::vector<std::int32_t> out(ids.begin(), ids.end());
    if (order_ == Order::alloc) std::ranges::stable_sort(out, std::greater{}, [&](std::int32_t i) { return nodes_[i].incl.alloc; });
    else if (order_ == Order::retained) std::ranges::stable_sort(out, std::greater{}, [&](std::int32_t i) { return nodes_[i].incl.retained; });
    else if (order_ == Order::time) std::ranges::stable_sort(out, std::greater{}, [&](std::int32_t i) { return nodes_[i].ev.end_ns - nodes_[i].ev.start_ns; });
    return out;
  }
  void walk(std::int32_t i, std::uint32_t depth, std::vector<Row>& out) const {
    if (hidden(i)) return;
    out.push_back({i, depth, true});
    if (!nodes_[i].expanded) return;
    for (std::int32_t c : ordered(nodes_[i].children)) walk(c, depth + 1, out);
  }
  void walk_filtered(std::int32_t i, std::uint32_t depth, const std::vector<char>& keep, std::vector<Row>& out) const {
    if (!keep[i] || hidden(i)) return;
    out.push_back({i, depth, keep[i] == 2});
    for (std::int32_t c : ordered(nodes_[i].children)) {
      if (keep[i] == 2 && nodes_[i].expanded) walk(c, depth + 1, out);
      else walk_filtered(c, depth + 1, keep, out);
    }
  }

  std::vector<std::int32_t> ordered_roots() const {
    std::vector<std::int32_t> out = roots_;
    if (newest_first_) std::ranges::reverse(out);
    return ordered(out);
  }

  std::vector<Node> nodes_;
  std::vector<std::int32_t> roots_;  // in id order
  bool newest_first_ = false;
  bool hide_reused_ = false;
  Order order_ = Order::start;
  std::optional<BuildMemory> memory_;
  std::set<std::uint64_t> computed_with_errors_;  // cells some node computed with errors
  std::unordered_map<std::uint64_t, std::int32_t> by_id_;
  std::unordered_map<std::uint64_t, std::vector<Waiting>> waiting_;  // by the parent's id they wait for
  std::vector<std::int32_t> running_;                               // nodes that may still run
  std::int32_t last_ = -1;                                          // the node that came last
  std::uint64_t resets_ = 0;
  bool folding_ = true;                                             // false while a batch is added
  mutable Summary summary_;
  mutable bool summary_dirty_ = false;
  std::uint64_t version_ = 0;
  mutable std::vector<Row> rows_;
  mutable std::uint64_t rows_at_ = 0;
  mutable bool rows_valid_ = false;
  std::vector<std::string> words_;
  std::vector<std::string> raw_words_;  // as typed, for paths
  std::optional<std::int32_t> worker_;
};

// The timeline: one lane per worker (lane 0 is the requesting thread, lane k+1 worker k), cut
// into `columns` equal slices of the trace's span. A slice shows the deepest event the lane was
// in during it (an event belongs to the worker that started it), or none.
struct Timeline {
  std::uint64_t span_ns = 0;
  std::vector<std::vector<std::int32_t>> lanes;  // node index per column, -1 for idle
  // The lane of a worker index.
  static std::size_t lane_of(std::int32_t worker) { return static_cast<std::size_t>(worker + 1); }
  static std::int32_t worker_of(std::size_t lane) { return static_cast<std::int32_t>(lane) - 1; }
  // The slice's time range.
  std::pair<std::uint64_t, std::uint64_t> slice(std::size_t col, std::size_t columns) const {
    return {span_ns * col / columns, span_ns * (col + 1) / columns};
  }
};

// The stream mode's printer: each batch's lines as they come, then a summary line.
class Stream {
 public:
  Stream(std::ostream& out, Style style) : out_(&out), style_(style) {}
  void batch(std::span<const Event> events) {
    std::string text;
    const Origins o = error_origins(events);
    const std::vector<Rollup> r = rollups(events);
    const std::vector<std::uint64_t> self = self_times(events);
    const auto parents = parents_in(events);
    // The outermost cell of its group on its path (no ancestor in the batch has the same name).
    // Names are compared in place; the walk stops after as many steps as there are events, so a
    // cycle of parent ids from the wire cannot hang it.
    const auto outermost = [&](std::size_t i, auto name_of) {
      const std::string_view name = name_of(events[i]);
      std::size_t steps = 0;
      for (auto p = parents[i]; p && steps < events.size(); p = parents[*p], ++steps)
        if (name_of(events[*p]) == name) return false;
      return true;
    };
    for (std::size_t i = 0; i < events.size(); ++i) {
      const Event& e = events[i];
      const std::optional<std::uint64_t> at = o.at[i] ? std::optional(events[*o.at[i]].id) : std::nullopt;
      const std::string_view below = o.at[i] ? first_line(events[*o.at[i]].errors) : first_line(e.errors);
      text += style_.json ? json_line(e, o.here[i], at, {}, r[i]) : stream_line(e, style_, o.here[i], below, r[i]);
      text += '\n';
      if (std::to_underlying(e.outcome) < outcome_count) ++summary_.counts[std::to_underlying(e.outcome)];
      ++summary_.events;
      summary_.span_ns = std::max(summary_.span_ns, e.end_ns);
      summary_.add_memory(e);
      kinds_.add(e);
      const auto kind_of = [](const Event& x) { return std::string_view(x.kind); };
      by_kind_.add(e.kind, e.kind, metrics_of(e, self[i], outermost(i, kind_of)), e.repeat);
      by_file_.add(file_group(e), file_group(e), metrics_of(e, self[i], outermost(i, file_group_name)), e.repeat);
    }
    *out_ << text << std::flush;
  }
  // A line of its own (a watched request's start and end).
  void note(std::string_view text) {
    if (style_.json) *out_ << R"({"note":)" << json_string(text) << "}\n" << std::flush;
    else *out_ << sgr(style_, "1;2", text) << '\n' << std::flush;
  }
  // `trace: 3 executed, 1 cutoff, 9 reused in 1.24ms` (without the time when times are off).
  std::string summary_line(std::optional<std::uint64_t> total_ns = std::nullopt) const {
    std::string out = "trace:";
    bool first = true;
    for (std::uint8_t i = 0; i < outcome_count; ++i) {
      if (summary_.counts[i] == 0) continue;
      const auto o = static_cast<Outcome>(i);
      out += std::format("{} {}", first ? "" : ",", sgr(style_, outcome_sgr(o), std::format("{} {}", summary_.counts[i], outcome_name(o))));
      first = false;
    }
    if (first) out += " no engine requests";
    if (style_.times) out += sgr(style_, "2", std::format(" in {}", format_duration(total_ns.value_or(summary_.span_ns))));
    return out;
  }
  // `memory: 12.3MiB allocated in 45210 allocations, 3.10MiB retained; process 40.0MiB, peak
  // 52.0MiB` and the kinds that allocated most, `by kind: Tu 5.10MiB, Pp 3.20MiB, …` (#274).
  // Empty when times are off (a golden test) or nothing was counted.
  std::string memory_lines(const std::optional<BuildMemory>& m = std::nullopt) const {
    if (!style_.times || (summary_.alloc_bytes == 0 && summary_.retained_bytes == 0 && !m)) return {};
    std::string out = std::format("memory: {} allocated in {} allocations, {} retained", format_bytes(static_cast<std::int64_t>(summary_.alloc_bytes)),
                                  summary_.allocs, format_bytes(summary_.retained_bytes));
    if (m) out += std::format("; process {}, peak {}", format_bytes(m->process_current), format_bytes(m->process_peak));
    out = sgr(style_, "2", out);
    const std::vector<KindSum> ks = kinds_.sorted();
    if (!ks.empty()) {
      std::string line = "by kind:";
      for (std::size_t i = 0; i < ks.size() && i < top_kinds; ++i)
        line += std::format("{} {} {}", i == 0 ? "" : ",", ks[i].kind, format_bytes(static_cast<std::int64_t>(ks[i].alloc_bytes)));
      if (ks.size() > top_kinds) line += ", …";
      out += '\n' + sgr(style_, "2", line);
    }
    return out;
  }
  static constexpr std::size_t top_kinds = 6;
  // `{"summary":{"executed":3,…,"events":12,"total_ns":…,"memory":{…},"kinds":[…]}}` in JSON
  // (docs/TRACE.md).
  std::string summary_json(std::optional<std::uint64_t> total_ns = std::nullopt, const std::optional<BuildMemory>& m = std::nullopt) const {
    std::string out = R"({"summary":{)";
    for (std::uint8_t i = 0; i < outcome_count; ++i)
      std::format_to(std::back_inserter(out), R"("{}":{},)", outcome_name(static_cast<Outcome>(i)), summary_.counts[i]);
    std::format_to(std::back_inserter(out), R"("events":{},"total_ns":{},"memory":{{"alloc_bytes":{},"allocs":{},"retained_bytes":{})",
                   summary_.events, total_ns.value_or(summary_.span_ns), summary_.alloc_bytes, summary_.allocs, summary_.retained_bytes);
    if (m) std::format_to(std::back_inserter(out), R"(,"process_current_bytes":{},"process_peak_bytes":{})", m->process_current, m->process_peak);
    out += R"(},"kinds":[)";
    bool first = true;
    for (const KindSum& k : kinds_.sorted()) {
      std::format_to(std::back_inserter(out), R"({}{{"kind":{},"executions":{},"alloc_bytes":{},"allocs":{},"peak_bytes":{},"retained_bytes":{}}})",
                     first ? "" : ",", json_string(k.kind), k.executions, k.alloc_bytes, k.allocs, k.peak_bytes, k.retained_bytes);
      first = false;
    }
    // Histograms (#637): the browser's groups, by kind and by file, most self time first.
    out += R"(],"histogram":{"by_kind":)" + groups_json(by_kind_.sorted(Metric::self_time), "kind");
    out += R"(,"by_file":)" + groups_json(by_file_.sorted(Metric::self_time), "file");
    return out + "}}}";
  }
  void summary(std::optional<std::uint64_t> total_ns = std::nullopt, const std::optional<BuildMemory>& m = std::nullopt) {
    if (style_.json) {
      *out_ << summary_json(total_ns, m) << '\n' << std::flush;
      return;
    }
    std::string text = summary_line(total_ns);
    if (const std::string mem = memory_lines(m); !mem.empty()) text += '\n' + mem;
    *out_ << text << '\n' << std::flush;
  }
  void reset() {
    summary_ = {};
    kinds_ = {};
    by_kind_ = {};
    by_file_ = {};
  }

 private:
  std::ostream* out_;
  Style style_;
  Summary summary_;
  Kinds kinds_;
  Groups by_kind_, by_file_;
};

// Times made relative to `base_ns` (a request's start on the daemon's clock).
void rebase(std::span<Event> events, std::uint64_t base_ns) {
  for (Event& e : events) {
    e.start_ns = e.start_ns > base_ns ? e.start_ns - base_ns : 0;
    e.end_ns = e.end_ns > base_ns ? e.end_ns - base_ns : 0;
  }
}

// The browser's histogram: groups over the cells the tree's filter keeps (requests' nodes aside),
// or only those in `within`'s subtree. By children: one group per child of `within` (or per top
// node), over its subtree.
enum class Grouping : std::uint8_t { kind, file, children };
inline constexpr std::size_t grouping_count = 3;
std::vector<Group> histogram(const Tree& t, Grouping by, std::optional<std::int32_t> within, Metric order) {
  const auto nodes = t.nodes();
  const auto node = [&](std::int32_t i) -> const Tree::Node& { return nodes[static_cast<std::size_t>(i)]; };
  // Self times from each node's children.
  const auto self = [&](std::int32_t i) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> kids;
    for (std::int32_t c : node(i).children) kids.emplace_back(node(c).ev.start_ns, node(c).ev.end_ns);
    return uncovered(node(i).ev.start_ns, node(i).ev.end_ns, std::move(kids));
  };
  Groups groups;
  // `path`: the groups that counted time in all above this node, so a group nested in itself
  // counts it once. By children, a group's key is its child node.
  const auto walk = [&](this const auto& walk, std::int32_t i, std::int32_t top, std::vector<std::string>& path) -> void {
    const Tree::Node& n = node(i);
    bool pushed = false;
    if (!n.group && t.matches(n.ev)) {
      const std::string key = by == Grouping::kind ? n.ev.kind : by == Grouping::file ? file_group(n.ev) : std::to_string(top);
      const std::string name = by == Grouping::children ? cell_name(node(top).ev) : key;
      const bool outer = std::ranges::find(path, key) == path.end();
      groups.add(key, name, metrics_of(n.ev, self(i), outer), n.ev.repeat).node = by == Grouping::children ? top : -1;
      if (outer) path.push_back(key), pushed = true;
    }
    for (std::int32_t c : n.children) walk(c, top, path);
    if (pushed) path.pop_back();
  };
  std::vector<std::int32_t> tops;
  if (within) tops = by == Grouping::children ? node(*within).children : std::vector<std::int32_t>{*within};
  else tops.assign(t.roots().begin(), t.roots().end());
  // A request's node holds a build: its children are the groups.
  if (!within && by == Grouping::children && tops.size() == 1 && node(tops[0]).group) tops = node(tops[0]).children;
  for (std::int32_t top : tops) {
    std::vector<std::string> path;
    walk(top, top, path);
  }
  return groups.sorted(order);
}

Timeline timeline(const Tree& t, std::size_t columns) {
  Timeline tl;
  const Summary s = t.summary();
  tl.span_ns = std::max<std::uint64_t>(s.span_ns, 1);
  tl.lanes.assign(static_cast<std::size_t>(s.workers) + 1, std::vector<std::int32_t>(columns, -1));
  if (columns == 0) return tl;
  std::vector<std::vector<std::uint32_t>> depth(tl.lanes.size(), std::vector<std::uint32_t>(columns, 0));
  const auto nodes = t.nodes();
  // The deepest wins a slice; of two as deep, the one that started first (then the lower id), so
  // the timeline does not depend on the order the nodes came in (#939).
  const auto before = [&](std::size_t a, std::int32_t b) {
    const Event &x = nodes[a].ev, &y = nodes[static_cast<std::size_t>(b)].ev;
    return std::tuple(x.start_ns, x.id) < std::tuple(y.start_ns, y.id);
  };
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const Event& e = nodes[i].ev;
    if (nodes[i].group || !nodes[i].attached || nodes[i].folded_into >= 0 || !t.matches(e)) continue;
    auto& lane = tl.lanes[Timeline::lane_of(e.worker)];
    auto& d = depth[Timeline::lane_of(e.worker)];
    const std::size_t from = static_cast<std::size_t>(e.start_ns * columns / tl.span_ns);
    const std::size_t to = std::max(from + 1, static_cast<std::size_t>((e.end_ns * columns + tl.span_ns - 1) / tl.span_ns));
    for (std::size_t c = std::min(from, columns - 1); c < std::min(to, columns); ++c) {
      if (lane[c] < 0 || e.depth + 1 > d[c] || (e.depth + 1 == d[c] && before(i, lane[c]))) {
        lane[c] = static_cast<std::int32_t>(i);
        d[c] = e.depth + 1;
      }
    }
  }
  return tl;
}

}  // namespace ycxx::trace
