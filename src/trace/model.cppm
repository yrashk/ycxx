// The trace as values (#245): one `Event` per cell a request settled, the line both modes render,
// and the browser's model: a tree with expansion, a filter by text, outcome or worker, the summary
// header, and the timeline of the pool's workers. Nothing here touches a terminal.
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
  std::string value;  // the kind's `show`, when values were asked for
  bool operator==(const Event&) const = default;
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
// SGR parameters: executed red, cutoff yellow, verified green, reused bright green, durable
// blue, inputs dim, recomputed bright red, waited cyan, failed bold magenta.
constexpr std::string_view outcome_sgr(Outcome o) {
  switch (o) {
    case Outcome::executed: return "31";
    case Outcome::cutoff: return "33";
    case Outcome::verified: return "32";
    case Outcome::reused: return "92";
    case Outcome::durable: return "34";
    case Outcome::input: return "90";
    case Outcome::recomputed: return "91";
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
struct Style {
  bool color = false;
  bool times = true;
};

std::string sgr(const Style& s, std::string_view params, std::string_view text) {
  if (!s.color) return std::string(text);
  return std::format("\x1b[{}m{}\x1b[0m", params, text);
}

// `Kind(key)`: the cell, as both modes name it.
std::string cell_name(const Event& e) { return e.key.empty() ? e.kind : std::format("{}({})", e.kind, e.key); }

// The stream's line: glyph, indentation by depth, the cell, its outcome, and its time and worker.
//   ●   Tu(a.cpp) executed 1.24ms w2
std::string stream_line(const Event& e, const Style& s) {
  std::string out = sgr(s, outcome_sgr(e.outcome), outcome_glyph(e.outcome, s.color));
  out += ' ';
  out.append(2 * e.depth, ' ');
  out += sgr(s, "1", e.kind);
  if (!e.key.empty()) out += std::format("({})", e.key);
  out += ' ';
  out += sgr(s, outcome_sgr(e.outcome), outcome_name(e.outcome));
  if (s.times) {
    const bool instant = e.end_ns == e.start_ns;
    out += sgr(s, "2", instant ? std::format(" {}", format_workers(e)) : std::format(" {} {}", format_duration(e.end_ns - e.start_ns), format_workers(e)));
  }
  return out;
}

// Counts per outcome and the span of the trace.
struct Summary {
  std::array<std::uint64_t, outcome_count> counts{};
  std::uint64_t events = 0;
  std::uint64_t span_ns = 0;  // from the trace's start to its last event's end
  std::int32_t workers = 0;   // pool workers seen
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
  };
  struct Row {
    std::int32_t node;
    std::uint32_t depth;
    bool match;  // matches the filter itself (else shown as an ancestor of a match)
  };

  // A request's node (`ctl trace`), under which `add(batch, id)` puts its top-level events.
  void open_group(std::uint64_t id, std::string label, std::uint64_t start_ns) {
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
  void annotate(std::uint64_t id, std::string text) {
    if (const auto it = by_id_.find(id); it != by_id_.end()) nodes_[it->second].ev.value = std::move(text);
  }
  // A request's end: its span, and its outcome: executed if anything under it ran.
  void close_group(std::uint64_t id, std::uint64_t end_ns) {
    if (const auto it = by_id_.find(id); it != by_id_.end()) {
      Node& n = nodes_[it->second];
      n.ev.end_ns = std::max(n.ev.start_ns, end_ns);
      n.ev.outcome = n.executions > 0 ? Outcome::executed : Outcome::reused;
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
      // Top-level nodes and the first levels below them start open, the rest closed.
      n.expanded = e.depth < open_depth;
      if (const auto it = by_id_.find(e.parent); e.parent != 0 && it != by_id_.end()) {
        n.parent = it->second;
        nodes_[it->second].children.push_back(at);
      } else {
        roots_.push_back(at);
      }
      by_id_.emplace(e.id, at);
      nodes_.push_back(std::move(n));
      for (std::int32_t p = nodes_[at].parent; p >= 0; p = nodes_[p].parent) {
        ++nodes_[p].subtree;
        nodes_[p].executions += nodes_[at].executions;
      }
    }
  }

  std::span<const Node> nodes() const noexcept { return nodes_; }
  const Node& node(std::int32_t i) const { return nodes_[i]; }
  std::span<const std::int32_t> roots() const noexcept { return roots_; }

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
      for (std::int32_t r : roots_) walk(r, 0, out);
      return out;
    }
    std::vector<char> keep(nodes_.size(), 0);  // 1: an ancestor of a match, 2: a match
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (!matches(nodes_[i].ev)) continue;
      keep[i] = 2;
      for (std::int32_t p = nodes_[i].parent; p >= 0 && !keep[p]; p = nodes_[p].parent) keep[p] = 1;
    }
    for (std::int32_t r : roots_) walk_filtered(r, 0, keep, out);
    return out;
  }

  Summary summary() const {
    Summary s;
    std::int32_t top = -1;
    for (const Node& n : nodes_) {
      if (n.group) continue;
      ++s.counts[std::to_underlying(n.ev.outcome)];
      ++s.events;
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
  void walk(std::int32_t i, std::uint32_t depth, std::vector<Row>& out) const {
    out.push_back({i, depth, true});
    if (!nodes_[i].expanded) return;
    for (std::int32_t c : nodes_[i].children) walk(c, depth + 1, out);
  }
  void walk_filtered(std::int32_t i, std::uint32_t depth, const std::vector<char>& keep, std::vector<Row>& out) const {
    if (!keep[i]) return;
    out.push_back({i, depth, keep[i] == 2});
    for (std::int32_t c : nodes_[i].children) {
      if (keep[i] == 2 && nodes_[i].expanded) walk(c, depth + 1, out);
      else walk_filtered(c, depth + 1, keep, out);
    }
  }

  std::vector<Node> nodes_;
  std::vector<std::int32_t> roots_;
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
    for (const Event& e : events) {
      text += stream_line(e, style_);
      text += '\n';
      if (std::to_underlying(e.outcome) < outcome_count) ++summary_.counts[std::to_underlying(e.outcome)];
      ++summary_.events;
      summary_.span_ns = std::max(summary_.span_ns, e.end_ns);
    }
    *out_ << text << std::flush;
  }
  // A line of its own (a watched request's start and end).
  void note(std::string_view text) { *out_ << sgr(style_, "1;2", text) << '\n' << std::flush; }
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
  void summary(std::optional<std::uint64_t> total_ns = std::nullopt) { *out_ << summary_line(total_ns) << '\n' << std::flush; }
  void reset() { summary_ = {}; }

 private:
  std::ostream* out_;
  Style style_;
  Summary summary_;
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
