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
  bool operator==(const Event&) const = default;
};

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
std::int64_t counted_retained(const Event& e) { return settles(e.outcome) ? std::max<std::int64_t>(0, e.retained_bytes) : 0; }

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
std::string_view event_sgr(const Event& e) { return e.errors.empty() ? outcome_sgr(e.outcome) : error_sgr; }

// `here`: the errors start at this cell (none of its children has any), so the whole line is
// marked, and its messages follow on their own lines.
std::string stream_line(const Event& e, const Style& s, bool here = false, std::string_view below = {}, const Rollup& r = {}) {
  std::string out = sgr(s, event_sgr(e), outcome_glyph(e.outcome, s.color));
  out += ' ';
  out.append(2 * e.depth, ' ');
  std::string body = sgr(s, here ? "" : "1", e.kind);
  if (!e.key.empty()) body += std::format("({})", e.key);
  body += ' ';
  body += here ? std::string(outcome_name(e.outcome)) : sgr(s, event_sgr(e), outcome_name(e.outcome));
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
  return out + "}";
}

// Counts per outcome and the span of the trace.
struct Summary {
  std::array<std::uint64_t, outcome_count> counts{};
  std::uint64_t events = 0;
  std::uint64_t span_ns = 0;  // from the trace's start to its last event's end
  std::int32_t workers = 0;   // pool workers seen
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

// The browser's tree. Events arrive in collector batches (pre-order); a node's children are in
// arrival order, which is their start order.
class Tree {
 public:
  struct Node {
    Event ev;
    std::int32_t parent = -1;  // index, -1 at the top
    std::vector<std::int32_t> children;
    bool expanded = false;
    bool group = false;  // a request of a watched daemon, holding its top-level events
    // In this node's subtree (itself included): events, and executions (executed or cutoff).
    std::uint64_t subtree = 1;
    std::uint64_t executions = 0;
    Rollup incl;  // memory in its subtree, itself included (#274)
  };
  // How siblings are ordered: as they started, or most memory first (#274): bytes allocated in
  // the subtree, or retained in it.
  enum class Order : std::uint8_t { start, alloc, retained };
  struct Row {
    std::int32_t node;
    std::uint32_t depth;
    bool match;  // matches the filter itself (else shown as an ancestor of a match)
  };

  // Hides reused cells (leaves only: a request's node stays), to see what ran.
  void set_hide_reused(bool on) noexcept { hide_reused_ = on; }
  bool hide_reused() const noexcept { return hide_reused_; }

  // Top-level nodes newest first (the live views' default: the latest request on top).
  void set_newest_first(bool on) noexcept { newest_first_ = on; }
  bool newest_first() const noexcept { return newest_first_; }

  void set_order(Order o) noexcept { order_ = o; }
  Order order() const noexcept { return order_; }

  // The latest build's totals beyond its events, for the header (#274).
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
    by_id_.emplace(id, static_cast<std::int32_t>(nodes_.size()));
    roots_.push_back(static_cast<std::int32_t>(nodes_.size()));
    nodes_.push_back(std::move(n));
  }
  // Text for a request's node (`--watch`: its diagnostics), shown in the detail pane.
  void annotate(std::uint64_t id, std::string text, bool append = false) {
    if (const auto it = by_id_.find(id); it != by_id_.end()) {
      std::string& v = nodes_[it->second].ev.value;
      v = append ? v + text : std::move(text);
    }
  }
  // A request's end: its span, and its outcome: executed if anything under it ran.
  void close_group(std::uint64_t id, std::uint64_t end_ns) {
    if (const auto it = by_id_.find(id); it != by_id_.end()) {
      Node& n = nodes_[it->second];
      n.ev.end_ns = std::max(n.ev.start_ns, end_ns);
      n.ev.outcome = n.executions > 0 ? Outcome::executed : Outcome::reused;
      // Red too when its build's values hold errors (they are shown below it).
      if (std::ranges::any_of(n.children, [&](std::int32_t c) { return !nodes_[c].ev.errors.empty(); }))
        n.ev.errors = "the build has errors";
    }
  }

  // Events in collector order; with `group`, top-level ones go under that request's node.
  void add(std::span<const Event> batch, std::uint64_t group = 0) {
    for (const Event& given : batch) {
      Event e = given;
      // From the wire: an outcome and workers in range (lanes are indexed by worker).
      if (std::to_underlying(e.outcome) >= outcome_count) e.outcome = Outcome::failed;
      e.worker = std::clamp(e.worker, -1, max_worker);
      e.end_worker = std::clamp(e.end_worker, -1, max_worker);
      if (e.parent == 0 || !by_id_.contains(e.parent)) e.parent = group;
      const auto at = static_cast<std::int32_t>(nodes_.size());
      Node n;
      n.ev = e;
      n.executions = e.outcome == Outcome::executed || e.outcome == Outcome::cutoff;
      n.incl = {e.alloc_bytes, counted_retained(e)};
      // Top-level nodes and the first levels below them start open, the rest closed.
      n.expanded = e.depth < open_depth;
      if (const auto it = by_id_.find(e.parent); e.parent != 0 && it != by_id_.end()) {
        n.parent = it->second;
        nodes_[it->second].children.push_back(at);
      } else {
        roots_.push_back(at);
      }
      by_id_.emplace(e.id, at);
      if (computed(e.outcome) && !e.errors.empty()) computed_with_errors_.insert(e.cell);
      nodes_.push_back(std::move(n));
      for (std::int32_t p = nodes_[at].parent; p >= 0; p = nodes_[p].parent) {
        ++nodes_[p].subtree;
        nodes_[p].executions += nodes_[at].executions;
        nodes_[p].incl.alloc += nodes_[at].incl.alloc;
        nodes_[p].incl.retained += nodes_[at].incl.retained;
      }
    }
  }

  std::span<const Node> nodes() const noexcept { return nodes_; }
  const Node& node(std::int32_t i) const { return nodes_[i]; }
  std::span<const std::int32_t> roots() const noexcept { return roots_; }

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
    if (!nodes_[i].children.empty()) nodes_[i].expanded = !nodes_[i].expanded;
  }
  void set_expanded(std::int32_t i, bool open) { nodes_[i].expanded = open && !nodes_[i].children.empty(); }
  void expand_all(bool open) {
    for (Node& n : nodes_) n.expanded = open && !n.children.empty();
  }
  // Opens every ancestor of `i`, so it is visible.
  void reveal(std::int32_t i) {
    for (std::int32_t p = nodes_[i].parent; p >= 0; p = nodes_[p].parent) nodes_[p].expanded = true;
  }

  // The filter: words that must all match. `word` matches the kind or key (case-insensitively),
  // `is:executed` (or a prefix, `is:ex`) an outcome, `w:3` or `w:main` a worker. Empty: all.
  void set_filter(std::string_view text) {
    words_.clear();
    for (auto w : text | std::views::split(' ')) {
      std::string_view word(w.begin(), w.end());
      if (!word.empty()) words_.emplace_back(lower(word));
    }
  }
  // Only events a worker ran (`std::nullopt`: any), on top of the text filter.
  void set_worker(std::optional<std::int32_t> w) { worker_ = w; }
  std::optional<std::int32_t> worker() const noexcept { return worker_; }
  bool filtering() const noexcept { return !words_.empty() || worker_.has_value(); }

  bool matches(const Event& e) const {
    if (worker_ && e.worker != *worker_ && e.end_worker != *worker_) return false;
    if (words_.empty()) return true;
    const std::string kind = lower(e.kind), key = lower(e.key);
    for (const std::string& w : words_) {
      if (w.starts_with("is:")) {
        const auto o = outcome_named(std::string_view(w).substr(3));
        if (!o || *o != e.outcome) return false;
      } else if (w.starts_with("w:")) {
        const std::optional<std::int32_t> n = worker_named(std::string_view(w).substr(2));
        if (!n || (e.worker != *n && e.end_worker != *n)) return false;
      } else if (!kind.contains(w) && !key.contains(w)) {
        return false;
      }
    }
    return true;
  }

  // The rows to draw. Unfiltered: what expansion shows. Filtered: every match, under its
  // ancestors (which are shown whether expanded or not), and a match's own subtree as expanded.
  std::vector<Row> rows() const {
    std::vector<Row> out;
    if (!filtering()) {
      for (std::int32_t r : ordered_roots()) walk(r, 0, out);
      return out;
    }
    std::vector<char> keep(nodes_.size(), 0);  // 1: an ancestor of a match, 2: a match
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (!matches(nodes_[i].ev)) continue;
      keep[i] = 2;
      for (std::int32_t p = nodes_[i].parent; p >= 0 && !keep[p]; p = nodes_[p].parent) keep[p] = 1;
    }
    for (std::int32_t r : ordered_roots()) walk_filtered(r, 0, keep, out);
    return out;
  }

  Summary summary() const {
    Summary s;
    std::int32_t top = -1;
    for (const Node& n : nodes_) {
      if (n.group) continue;
      ++s.counts[std::to_underlying(n.ev.outcome)];
      ++s.events;
      s.add_memory(n.ev);
      s.span_ns = std::max(s.span_ns, n.ev.end_ns);
      top = std::max({top, n.ev.worker, n.ev.end_worker});
    }
    s.workers = top + 1;
    return s;
  }

  static constexpr std::uint32_t open_depth = 2;
  static constexpr std::int32_t max_worker = 1023;

 private:
  static std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
  }
  bool errors_below(std::int32_t i) const {
    return std::ranges::any_of(nodes_[i].children, [&](std::int32_t c) { return !nodes_[c].ev.errors.empty() || errors_below(c); });
  }
  bool hidden(std::int32_t i) const {
    const Node& n = nodes_[i];
    return hide_reused_ && !n.group && n.children.empty() && n.ev.outcome == Outcome::reused;
  }
  // Siblings in the chosen order; ties keep their start order.
  std::vector<std::int32_t> ordered(std::span<const std::int32_t> ids) const {
    std::vector<std::int32_t> out(ids.begin(), ids.end());
    if (order_ == Order::alloc) std::ranges::stable_sort(out, std::greater{}, [&](std::int32_t i) { return nodes_[i].incl.alloc; });
    else if (order_ == Order::retained) std::ranges::stable_sort(out, std::greater{}, [&](std::int32_t i) { return nodes_[i].incl.retained; });
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
  std::vector<std::int32_t> roots_;
  bool newest_first_ = false;
  bool hide_reused_ = false;
  Order order_ = Order::start;
  std::optional<BuildMemory> memory_;
  std::set<std::uint64_t> computed_with_errors_;  // cells some node computed with errors
  std::unordered_map<std::uint64_t, std::int32_t> by_id_;
  std::vector<std::string> words_;
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
    return out + "]}}";
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
  }

 private:
  std::ostream* out_;
  Style style_;
  Summary summary_;
  Kinds kinds_;
};

// Times made relative to `base_ns` (a request's start on the daemon's clock).
void rebase(std::span<Event> events, std::uint64_t base_ns) {
  for (Event& e : events) {
    e.start_ns = e.start_ns > base_ns ? e.start_ns - base_ns : 0;
    e.end_ns = e.end_ns > base_ns ? e.end_ns - base_ns : 0;
  }
}

Timeline timeline(const Tree& t, std::size_t columns) {
  Timeline tl;
  const Summary s = t.summary();
  tl.span_ns = std::max<std::uint64_t>(s.span_ns, 1);
  tl.lanes.assign(static_cast<std::size_t>(s.workers) + 1, std::vector<std::int32_t>(columns, -1));
  if (columns == 0) return tl;
  std::vector<std::vector<std::uint32_t>> depth(tl.lanes.size(), std::vector<std::uint32_t>(columns, 0));
  const auto nodes = t.nodes();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const Event& e = nodes[i].ev;
    if (nodes[i].group || !t.matches(e)) continue;
    auto& lane = tl.lanes[Timeline::lane_of(e.worker)];
    auto& d = depth[Timeline::lane_of(e.worker)];
    const std::size_t from = static_cast<std::size_t>(e.start_ns * columns / tl.span_ns);
    const std::size_t to = std::max(from + 1, static_cast<std::size_t>((e.end_ns * columns + tl.span_ns - 1) / tl.span_ns));
    for (std::size_t c = std::min(from, columns - 1); c < std::min(to, columns); ++c) {
      if (lane[c] < 0 || e.depth + 1 > d[c]) {
        lane[c] = static_cast<std::int32_t>(i);
        d[c] = e.depth + 1;
      }
    }
  }
  return tl;
}

}  // namespace ycxx::trace
