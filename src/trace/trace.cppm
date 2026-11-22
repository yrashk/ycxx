// What went through the query engine, for `ycxx cxx --trace-engine` (#245): the engine's events
// as values that cross the daemon socket, a collector that turns a request's concurrent events
// into complete subtrees in dependency order, the line renderer of the stream mode, and the model
// behind the interactive browser (`:browse`): the tree, its filters, the summary and the timeline
// of the pool's workers.
export module ycxx.trace;

import std;
import ycxx.engine;
export import :model;
export import :view;
export import :browse;

export namespace ycxx::trace {

// Turns a request's engine events into `Event`s and hands them to `sink` a complete subtree at a
// time, in pre-order with depths: when a top-level event (a request's own cell) arrives, every
// event under it has (frames report before they publish). Children follow their start order. An
// event whose parent was already handed on (a failed frame's child that was still running) goes
// on at once, at its depth. `sink` is called under the collector's lock, so batches never
// interleave, whichever workers report.
class Collector final : public engine::Tracer {
 public:
  using Sink = std::function<void(std::span<const Event>)>;
  explicit Collector(Sink sink, bool values = false, std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now())
      : sink_(std::move(sink)), values_(values), origin_(origin) {}

  void event(engine::TraceEvent e) noexcept override {
    Event ev = convert(e);
    std::lock_guard lock(m_);
    try {
      if (ev.parent == 0) {
        flush(std::move(ev), 0);
      } else if (const auto it = handed_.find(ev.parent); it != handed_.end()) {
        flush(std::move(ev), it->second + 1);
      } else {
        children_[ev.parent].push_back(ev.id);
        pending_.emplace(ev.id, std::move(ev));
      }
    } catch (...) {
      // A sink that throws (a closed socket) loses the trace, never the compile.
    }
  }
  bool values() const noexcept override { return values_; }

  // Hands on whatever never found its parent (nothing, unless a request ended early).
  void finish() {
    std::lock_guard lock(m_);
    std::vector<std::uint64_t> orphans;
    for (const auto& [id, ev] : pending_) {
      if (!pending_.contains(ev.parent)) orphans.push_back(id);
    }
    std::ranges::sort(orphans);
    for (std::uint64_t id : orphans) {
      auto node = pending_.extract(id);
      if (!node.empty()) flush(std::move(node.mapped()), 0);
    }
  }

  std::chrono::steady_clock::time_point origin() const noexcept { return origin_; }

 private:
  Event convert(const engine::TraceEvent& e) const {
    Event ev;
    ev.id = e.id;
    ev.parent = e.parent;
    ev.kind = short_kind(e.kind);
    ev.key = e.key;
    ev.outcome = e.outcome;
    const auto since = [&](std::chrono::steady_clock::time_point t) {
      return t < origin_ ? 0 : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t - origin_).count());
    };
    ev.start_ns = since(e.start);
    ev.end_ns = std::max(ev.start_ns, since(e.end));
    ev.worker = e.worker;
    ev.end_worker = e.end_worker;
    ev.value = e.value;
    ev.errors = e.errors;
    ev.cell = e.cell;
    ev.alloc_bytes = e.alloc_bytes;
    ev.allocs = e.allocs;
    ev.peak_bytes = e.peak_bytes;
    ev.retained_bytes = e.retained_bytes;
    ev.span = e.span;
    return ev;
  }

  // `ev` and everything pending under it, in pre-order.
  void flush(Event ev, std::uint32_t depth) {
    std::vector<Event> batch;
    gather(std::move(ev), depth, batch);
    sink_(batch);
  }
  void gather(Event ev, std::uint32_t depth, std::vector<Event>& out) {
    ev.depth = depth;
    const std::uint64_t id = ev.id;
    handed_.emplace(id, depth);
    out.push_back(std::move(ev));
    auto kids = children_.extract(id);
    if (kids.empty()) return;
    std::ranges::sort(kids.mapped());
    std::optional<std::size_t> last;  // the previous child, if it can take a repeat
    for (std::uint64_t k : kids.mapped()) {
      auto node = pending_.extract(k);
      if (node.empty()) continue;
      Event& e = node.mapped();
      // An instant just like the previous child (the same cell read again) is counted, not listed.
      const bool instant = e.start_ns == e.end_ns && !children_.contains(e.id);
      if (instant && last && same_instant(out[*last], e)) {
        ++out[*last].repeat;
        continue;
      }
      const std::size_t at = out.size();
      gather(std::move(e), depth + 1, out);
      last = instant && out.size() == at + 1 ? std::optional(at) : std::nullopt;
    }
  }

  static bool same_instant(const Event& a, const Event& b) {
    return a.cell == b.cell && a.kind == b.kind && a.outcome == b.outcome && a.errors == b.errors && a.start_ns == a.end_ns;
  }

  Sink sink_;
  bool values_;
  std::chrono::steady_clock::time_point origin_;
  std::mutex m_;
  std::unordered_map<std::uint64_t, Event> pending_;
  std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> children_;
  std::unordered_map<std::uint64_t, std::uint32_t> handed_;  // id -> depth
};

}  // namespace ycxx::trace
