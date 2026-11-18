// Building an L1 body, and the canonical order (optimization design §2.4).
//
// The builder takes blocks and nodes in any order: the front end's emission order. `finish`
// puts them in canonical order:
// - blocks in reverse post-order from the entry block (block 0), visiting successors in order;
//   unreachable blocks are dropped;
// - the nodes of each block in operand order: a post-order walk from the block's roots, visiting
//   each node's operands, then its successors' arguments, in order; a node that moves the state
//   chain on also waits for the block's readers of the state it consumes. The roots are the
//   nodes no other node of the block depends on, in emission order, then the terminator.
// Emission order thus only breaks ties between nodes no dataflow orders: dead nodes, and nodes
// used only in other blocks. Two front ends that emit the same dataflow in different orders get
// the same body, and so the same fingerprint. Floating nodes stay in the block they were emitted
// in; placing them (global code motion, §2.2) is the optimizer's.
export module ycxx.ir:builder;

import std;
import :handles;
import :ops;
import :function;

namespace ycxx::ir {

export struct SuccRef {
  BlockId target{};
  std::vector<ValueId> args;
};

// The result of `finish`: the body, and the canonical id of each emitted node (none if dropped),
// so the front end can remap what it recorded against emission ids (function_debug).
export struct Built {
  Function function;
  std::vector<NodeId> node_of;
};

export class Builder {
 public:
  BlockId add_block(std::span<const Ty> params) {
    BlockId b{std::uint32_t(blocks_.size())};
    auto& bl = blocks_.emplace_back();
    for (std::uint32_t i = 0; i < params.size(); ++i) {
      bl.params.push_back(ValueId(std::uint32_t(values_.size())));
      values_.push_back({params[i], true, raw(b), i});
    }
    return b;
  }
  BlockId add_block(std::initializer_list<Ty> params) { return add_block(std::span(params)); }

  ValueId param(BlockId b, std::uint32_t i) const { return blocks_[raw(b)].params[i]; }

  // Appends a node to block `b`. Operands and successor arguments may be `ValueId(none)` for now,
  // and set later with `set_operand` and `set_arg` (a parser meets uses before definitions).
  NodeId emit(BlockId b, Op op, Flags flags, std::span<const Ty> results,
              std::span<const ValueId> operands, std::span<const std::uint64_t> imms = {},
              std::span<const SuccRef> succs = {}) {
    NodeId n{std::uint32_t(nodes_.size())};
    RawNode& r = nodes_.emplace_back();
    r.block = raw(b);
    r.op = op;
    r.flags = flags;
    r.operands.assign(operands.begin(), operands.end());
    r.imms.assign(imms.begin(), imms.end());
    r.succs.assign(succs.begin(), succs.end());
    for (std::uint32_t i = 0; i < results.size(); ++i) {
      r.results.push_back(ValueId(std::uint32_t(values_.size())));
      values_.push_back({results[i], false, raw(n), i});
    }
    blocks_[raw(b)].nodes.push_back(n);
    return n;
  }
  NodeId emit(BlockId b, Op op, std::initializer_list<Ty> results,
              std::initializer_list<ValueId> operands, std::initializer_list<std::uint64_t> imms = {},
              Flags flags = 0) {
    return emit(b, op, flags, std::span(results), std::span(operands), std::span(imms));
  }

  ValueId result(NodeId n, std::uint32_t i = 0) const { return nodes_[raw(n)].results[i]; }
  void set_operand(NodeId n, std::uint32_t i, ValueId v) { nodes_[raw(n)].operands[i] = v; }
  void set_arg(NodeId n, std::uint32_t succ, std::uint32_t i, ValueId v) {
    nodes_[raw(n)].succs[succ].args[i] = v;
  }
  void set_target(NodeId n, std::uint32_t succ, BlockId b) { nodes_[raw(n)].succs[succ].target = b; }

  // Puts the body in canonical order. Fails on a block without exactly one terminator (last), a
  // cycle inside a block, an unset operand, or a use of a value from a dropped block. The result
  // still has to pass `verify`.
  std::expected<Built, std::string> finish() const;

 private:
  struct RawBlock {
    std::vector<ValueId> params;
    std::vector<NodeId> nodes;
  };
  struct RawNode {
    std::uint32_t block = 0;
    Op op{};
    Flags flags = 0;
    std::vector<ValueId> results, operands;
    std::vector<std::uint64_t> imms;
    std::vector<SuccRef> succs;
  };
  std::vector<RawBlock> blocks_;
  std::vector<RawNode> nodes_;
  std::vector<Value> values_;
};

std::expected<Built, std::string> Builder::finish() const {
  using std::uint32_t;
  auto fail = [](std::string s) { return std::unexpected(std::move(s)); };
  if (blocks_.empty()) return fail("a function needs an entry block");
  for (uint32_t b = 0; b < blocks_.size(); ++b) {
    const auto& ns = blocks_[b].nodes;
    if (ns.empty() || !is_terminator(nodes_[raw(ns.back())].op))
      return fail(std::format("block {} does not end with a terminator", b));
    for (std::size_t i = 0; i + 1 < ns.size(); ++i)
      if (is_terminator(nodes_[raw(ns[i])].op))
        return fail(std::format("block {} has a terminator before its end", b));
    for (const SuccRef& s : nodes_[raw(ns.back())].succs)
      if (raw(s.target) >= blocks_.size()) return fail(std::format("block {} branches to no block", b));
  }

  // Blocks in reverse post-order, by an explicit stack.
  std::vector<uint32_t> post;
  std::vector<std::uint8_t> seen(blocks_.size());
  {
    std::vector<std::pair<uint32_t, uint32_t>> stack{{0, 0}};
    seen[0] = 1;
    while (!stack.empty()) {
      auto& [b, next] = stack.back();
      const auto& succs = nodes_[raw(blocks_[b].nodes.back())].succs;
      if (next < succs.size()) {
        uint32_t t = raw(succs[next++].target);
        if (!seen[t]) {
          seen[t] = 1;
          stack.push_back({t, 0});
        }
      } else {
        post.push_back(b);
        stack.pop_back();
      }
    }
  }
  std::vector<uint32_t> order(post.rbegin(), post.rend());
  std::vector<uint32_t> block_of(blocks_.size(), none);
  for (uint32_t i = 0; i < order.size(); ++i) block_of[order[i]] = i;

  // Nodes of each block in operand order. A node depends on the nodes of its block that define
  // its operands, then its successors' arguments. A node that consumes a state and moves the
  // chain on (it yields a state, or is a terminator) also depends on the block's readers of that
  // state (consumers that yield none, as `load` does), so a reader always sees the state it names.
  auto is_state = [&](ValueId v) {
    return raw(v) < values_.size() && values_[raw(v)].type.kind == TyKind::state;
  };
  auto moves_chain = [&](const RawNode& r) {
    return is_terminator(r.op) || std::ranges::any_of(r.results, is_state);
  };
  std::vector<std::vector<uint32_t>> readers(values_.size());
  for (uint32_t n = 0; n < nodes_.size(); ++n)
    if (!moves_chain(nodes_[n]))
      for (ValueId v : nodes_[n].operands)
        if (is_state(v)) readers[raw(v)].push_back(n);
  auto deps = [&](uint32_t n, uint32_t b) {
    const RawNode& r = nodes_[n];
    std::vector<uint32_t> out;
    auto def = [&](ValueId v) {
      if (raw(v) >= values_.size()) return;
      const Value& val = values_[raw(v)];
      if (!val.param && nodes_[val.def].block == b) out.push_back(val.def);
    };
    for (ValueId v : r.operands) def(v);
    for (const SuccRef& s : r.succs)
      for (ValueId v : s.args) def(v);
    if (moves_chain(r)) {
      auto chain = [&](ValueId v) {
        if (!is_state(v)) return;
        for (uint32_t x : readers[raw(v)])
          if (x != n && nodes_[x].block == b) out.push_back(x);
      };
      for (ValueId v : r.operands) chain(v);
      for (const SuccRef& s : r.succs)
        for (ValueId v : s.args) chain(v);
    }
    return out;
  };
  std::vector<uint32_t> node_order;
  node_order.reserve(nodes_.size());
  std::vector<std::uint8_t> state(nodes_.size());  // 0 new, 1 on the stack, 2 placed
  std::vector<std::uint8_t> used(nodes_.size());
  for (uint32_t b : order) {
    const auto& ns = blocks_[b].nodes;
    for (NodeId n : ns)
      for (uint32_t d : deps(raw(n), b)) used[d] = 1;
    std::vector<uint32_t> roots;
    for (std::size_t i = 0; i + 1 < ns.size(); ++i)
      if (!used[raw(ns[i])]) roots.push_back(raw(ns[i]));
    roots.push_back(raw(ns.back()));
    for (uint32_t root : roots) {
      if (state[root]) continue;
      // Each frame is a node, its dependencies, and how many of them have been visited.
      struct Frame {
        uint32_t node;
        std::vector<uint32_t> deps;
        std::size_t next = 0;
      };
      std::vector<Frame> stack;
      stack.push_back({root, deps(root, b)});
      state[root] = 1;
      while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next < top.deps.size()) {
          uint32_t d = top.deps[top.next++];
          if (state[d] == 2) continue;
          if (state[d] == 1) return fail(std::format("block {} has a dataflow cycle", b));
          state[d] = 1;
          stack.push_back({d, deps(d, b)});
        } else {
          state[top.node] = 2;
          node_order.push_back(top.node);
          stack.pop_back();
        }
      }
    }
    // A node no root reaches is used only by nodes of its own block that it also feeds.
    for (NodeId n : ns)
      if (state[raw(n)] != 2) return fail(std::format("block {} has a dataflow cycle", b));
  }

  // Renumber: values are block parameters then node results, block by block.
  Built out;
  Function& f = out.function;
  out.node_of.assign(nodes_.size(), NodeId(none));
  std::vector<uint32_t> value_of(values_.size(), none);
  std::size_t next_node = 0;
  for (uint32_t b : order) {
    Block& bl = f.blocks.emplace_back();
    bl.param0 = uint32_t(f.values.size());
    for (ValueId p : blocks_[b].params) {
      value_of[raw(p)] = uint32_t(f.values.size());
      Value v = values_[raw(p)];
      v.def = block_of[b];
      f.values.push_back(v);
    }
    bl.nparams = uint32_t(blocks_[b].params.size());
    bl.node0 = uint32_t(next_node);
    for (; next_node < node_order.size() && nodes_[node_order[next_node]].block == b; ++next_node) {
      uint32_t n = node_order[next_node];
      out.node_of[n] = NodeId(uint32_t(next_node));
      for (ValueId r : nodes_[n].results) {
        value_of[raw(r)] = uint32_t(f.values.size());
        Value v = values_[raw(r)];
        v.def = uint32_t(next_node);
        f.values.push_back(v);
      }
    }
    bl.nnodes = uint32_t(next_node) - bl.node0;
  }
  auto map = [&](ValueId v) -> std::expected<ValueId, std::string> {
    if (raw(v) >= values_.size()) return fail("an operand was never set");
    if (value_of[raw(v)] == none) return fail(std::format("%{} is defined in an unreachable block", raw(v)));
    return ValueId(value_of[raw(v)]);
  };
  uint32_t value_at = 0;
  for (uint32_t i = 0; i < node_order.size(); ++i) {
    const RawNode& r = nodes_[node_order[i]];
    Node& n = f.nodes.emplace_back();
    n.op = r.op;
    n.flags = r.flags;
    n.block = block_of[r.block];
    const Block& bl = f.blocks[n.block];
    if (i == bl.node0) value_at = bl.param0 + bl.nparams;
    n.res0 = value_at;
    n.nres = uint32_t(r.results.size());
    value_at += n.nres;
    n.opnd0 = uint32_t(f.operands.size());
    n.nopnd = uint32_t(r.operands.size());
    for (ValueId v : r.operands) {
      auto m = map(v);
      if (!m) return std::unexpected(m.error());
      f.operands.push_back(*m);
    }
    n.imm0 = uint32_t(f.imms.size());
    n.nimm = uint32_t(r.imms.size());
    f.imms.insert(f.imms.end(), r.imms.begin(), r.imms.end());
    n.succ0 = uint32_t(f.succs.size());
    n.nsucc = uint32_t(r.succs.size());
    for (const SuccRef& s : r.succs) {
      Succ& e = f.succs.emplace_back();
      e.target = BlockId(block_of[raw(s.target)]);
      e.arg0 = uint32_t(f.operands.size());
      e.nargs = uint32_t(s.args.size());
      for (ValueId v : s.args) {
        auto m = map(v);
        if (!m) return std::unexpected(m.error());
        f.operands.push_back(*m);
      }
    }
  }
  return out;
}

}  // namespace ycxx::ir
