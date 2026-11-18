// Debug information beside an L1 body (optimization design §2.4). `FunctionDebug` is the value of
// `function_debug(F, ctx)`, separate from `function_ir` so positions never reach the body's
// fingerprint. Every transformation from L1 to L3 carries an `OriginMap` from its new nodes to
// the `(function entity, node id)` pairs they came from; `tu(TU)` composes the maps and renders
// DWARF late.
export module ycxx.ir:debug;

import std;
import :handles;
import :function;

namespace ycxx::ir {

export struct Origin {
  EntityId function{};
  NodeId node{};
  constexpr bool operator==(const Origin&) const = default;
  constexpr auto operator<=>(const Origin& b) const {
    return std::pair(raw(function), raw(node)) <=> std::pair(raw(b.function), raw(b.node));
  }
};

// For each node of a transformed body, the set of origins it came from, sorted and without
// duplicates. A node with no origin (a spill, say) has an empty set.
export class OriginMap {
 public:
  OriginMap() = default;
  explicit OriginMap(std::span<const std::vector<Origin>> per_node) {
    start_.push_back(0);
    for (auto set : per_node) {
      std::ranges::sort(set);
      auto [first, last] = std::ranges::unique(set);
      set.erase(first, last);
      origins_.insert(origins_.end(), set.begin(), set.end());
      start_.push_back(std::uint32_t(origins_.size()));
    }
  }
  // The identity map of an L1 body: each node is its own origin.
  static OriginMap identity(EntityId function, std::uint32_t nodes) {
    std::vector<std::vector<Origin>> sets(nodes);
    for (std::uint32_t i = 0; i < nodes; ++i) sets[i] = {{function, NodeId(i)}};
    return OriginMap(sets);
  }

  std::uint32_t size() const { return start_.empty() ? 0 : std::uint32_t(start_.size() - 1); }
  std::span<const Origin> of(NodeId n) const {
    return std::span(origins_).subspan(start_[raw(n)], start_[raw(n) + 1] - start_[raw(n)]);
  }

  // `outer` maps nodes of C to nodes of the B bodies of some functions; `inner(F)` maps the nodes
  // of F's B body to their origins (nullptr: F's B body is its L1 body). The result maps C
  // directly to the origins.
  template <class Inner>
    requires std::is_invocable_r_v<const OriginMap*, Inner, EntityId>
  static OriginMap compose(const OriginMap& outer, Inner&& inner) {
    std::vector<std::vector<Origin>> sets(outer.size());
    for (std::uint32_t i = 0; i < outer.size(); ++i)
      for (const Origin& o : outer.of(NodeId(i))) {
        const OriginMap* m = inner(o.function);
        if (!m) {
          sets[i].push_back(o);
          continue;
        }
        auto deeper = m->of(o.node);
        sets[i].insert(sets[i].end(), deeper.begin(), deeper.end());
      }
    return OriginMap(sets);
  }

  bool operator==(const OriginMap&) const = default;

 private:
  std::vector<std::uint32_t> start_;
  std::vector<Origin> origins_;
};

export struct VarDebug {
  LocalId var{};
  std::vector<NodeId> nodes;  // the `alloc`s and values that hold the variable
  bool operator==(const VarDebug&) const = default;
};

export struct FunctionDebug {
  std::vector<Loc> loc;  // by node id
  std::vector<VarDebug> vars;
  bool operator==(const FunctionDebug&) const = default;

  // Re-keys debug information recorded against emission ids by `Built::node_of`. Nodes that were
  // dropped (unreachable) lose their entries.
  FunctionDebug remap(std::span<const NodeId> node_of, std::uint32_t nodes) const {
    FunctionDebug out;
    out.loc.assign(nodes, Loc{});
    for (std::uint32_t i = 0; i < loc.size() && i < node_of.size(); ++i)
      if (raw(node_of[i]) != none) out.loc[raw(node_of[i])] = loc[i];
    for (const VarDebug& v : vars) {
      VarDebug& w = out.vars.emplace_back(v.var);
      for (NodeId n : v.nodes)
        if (raw(n) < node_of.size() && raw(node_of[raw(n)]) != none) w.nodes.push_back(node_of[raw(n)]);
      std::ranges::sort(w.nodes, {}, [](NodeId n) { return raw(n); });
    }
    return out;
  }
};

}  // namespace ycxx::ir
