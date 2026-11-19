// An L1 function body (optimization design §2.2, §2.4): a CFG of blocks with arguments, whose
// nodes thread `!state` through effectful operations. A `Function` is always canonical: it is
// produced by `Builder::finish`, which orders blocks in reverse post-order and the nodes of each
// block by their operands (builder.cppm). Block, node and value ids are dense indexes in that
// order; the entry block is block 0 and its parameters are the function's (!state first).
//
// Storage is a set of flat arrays, so a body is cheap to intern, hash and compare.
export module ycxx.ir:function;

import std;
import :handles;
import :ops;

namespace ycxx::ir {

export enum class ValueId : std::uint32_t {};
export enum class NodeId : std::uint32_t {};
export enum class BlockId : std::uint32_t {};
export constexpr std::uint32_t none = ~0u;

export enum class TyKind : std::uint8_t { cxx, state, exn, coro };

export struct Ty {
  TyKind kind = TyKind::cxx;
  QualType type{};  // for `cxx`
  static constexpr Ty of(QualType t) { return {TyKind::cxx, t}; }
  static constexpr Ty state() { return {TyKind::state}; }
  static constexpr Ty exn() { return {TyKind::exn}; }
  static constexpr Ty coro() { return {TyKind::coro}; }
  constexpr bool operator==(const Ty&) const = default;
};

export struct Block {
  std::uint32_t param0 = 0, nparams = 0;  // values
  std::uint32_t node0 = 0, nnodes = 0;    // nodes; the last one is the terminator
  constexpr bool operator==(const Block&) const = default;
};

export struct Node {
  Op op{};
  Flags flags = 0;
  std::uint32_t block = 0;
  std::uint32_t res0 = 0, nres = 0;    // values
  std::uint32_t opnd0 = 0, nopnd = 0;  // Function::operands
  std::uint32_t imm0 = 0, nimm = 0;    // Function::imms
  std::uint32_t succ0 = 0, nsucc = 0;  // Function::succs
  constexpr bool operator==(const Node&) const = default;
};

export struct Succ {
  BlockId target{};
  std::uint32_t arg0 = 0, nargs = 0;  // Function::operands
  constexpr bool operator==(const Succ&) const = default;
};

export struct Value {
  Ty type;
  bool param = false;
  std::uint32_t def = 0;    // the block (a parameter) or the node (a result)
  std::uint32_t index = 0;  // which parameter or result
  constexpr bool operator==(const Value&) const = default;
};

// Immediates are 64-bit words. A scalar immediate is one word; a list (`path`, `consts`) is its
// length followed by its elements. A path step is (step << 32 | index).
export constexpr std::uint64_t encode(PathStep s) {
  return std::uint64_t(std::to_underlying(s.step)) << 32 | s.index;
}
export constexpr PathStep decode_step(std::uint64_t w) {
  return {Step(w >> 32), std::uint32_t(w)};
}
export constexpr bool is_list(Imm k) { return k == Imm::path || k == Imm::consts; }

// The parameters an edge's destination receives from the operation rather than from the edge's
// arguments, ahead of them: an unwind edge delivers (!state, !exn), a coroutine resumption or
// destruction delivers !state.
export constexpr std::span<const TyKind> implicit_params(Op op, std::uint32_t succ) {
  static constexpr TyKind unwind[] = {TyKind::state, TyKind::exn};
  static constexpr TyKind resumed[] = {TyKind::state};
  switch (op) {
    case Op::invoke:
    case Op::invoke_ind:
      if (succ == 1) return unwind;
      return {};
    case Op::throw_:
    case Op::rethrow:
    case Op::resume: return unwind;
    case Op::coro__suspend: return resumed;
    default: return {};
  }
}

export struct Function {
  // The entry block's parameters that are the addresses of objects of class type: a mask over
  // the value parameters (after `!state`) that are parameters of class type ([expr.call]/7), and
  // 1 + the index of the one the class result initializes ([stmt.return]), 0 if none. As the
  // call op's.
  std::uint32_t byval_params = 0;
  std::uint32_t result_param = 0;
  std::vector<Block> blocks;
  std::vector<Node> nodes;
  std::vector<Value> values;
  std::vector<ValueId> operands;
  std::vector<std::uint64_t> imms;
  std::vector<Succ> succs;

  bool operator==(const Function&) const = default;

  const Block& block(BlockId b) const { return blocks[raw(b)]; }
  const Node& node(NodeId n) const { return nodes[raw(n)]; }
  const Value& value(ValueId v) const { return values[raw(v)]; }

  auto block_ids() const {
    return std::views::iota(0u, std::uint32_t(blocks.size())) |
           std::views::transform([](std::uint32_t i) { return BlockId(i); });
  }
  auto nodes_of(BlockId b) const {
    const Block& bl = block(b);
    return std::views::iota(bl.node0, bl.node0 + bl.nnodes) |
           std::views::transform([](std::uint32_t i) { return NodeId(i); });
  }
  auto params_of(BlockId b) const {
    const Block& bl = block(b);
    return std::views::iota(bl.param0, bl.param0 + bl.nparams) |
           std::views::transform([](std::uint32_t i) { return ValueId(i); });
  }
  auto results_of(NodeId n) const {
    const Node& nd = node(n);
    return std::views::iota(nd.res0, nd.res0 + nd.nres) |
           std::views::transform([](std::uint32_t i) { return ValueId(i); });
  }
  std::span<const ValueId> operands_of(NodeId n) const {
    const Node& nd = node(n);
    return std::span(operands).subspan(nd.opnd0, nd.nopnd);
  }
  std::span<const std::uint64_t> imms_of(NodeId n) const {
    const Node& nd = node(n);
    return std::span(imms).subspan(nd.imm0, nd.nimm);
  }
  std::span<const Succ> succs_of(NodeId n) const {
    const Node& nd = node(n);
    return std::span(succs).subspan(nd.succ0, nd.nsucc);
  }
  std::span<const ValueId> args_of(const Succ& s) const {
    return std::span(operands).subspan(s.arg0, s.nargs);
  }
  NodeId terminator(BlockId b) const {
    const Block& bl = block(b);
    return NodeId(bl.node0 + bl.nnodes - 1);
  }
};

// Checks a body against the op table: operand, result and immediate shapes, flags, successors and
// their arguments, that every use is dominated by its definition, and that the state forms one
// chain: on every path a state is moved on at most once and read only before that. The first
// problem found is returned, naming the node.
export std::expected<void, std::string> verify(const Function& f);

}  // namespace ycxx::ir

namespace ycxx::ir {

namespace {

bool matches(Arg a, TyKind k) {
  switch (a) {
    case Arg::state: return k == TyKind::state;
    case Arg::exn: return k == TyKind::exn;
    case Arg::coro: return k == TyKind::coro;
    case Arg::value:
    case Arg::values: return k == TyKind::cxx;
  }
  return false;
}

// Checks the kinds of `vals` against `sig`; a trailing `values` takes any number of C++ values.
template <class R>
std::optional<std::string> check_sig(const Function& f, Sig sig, R&& vals, std::string_view what) {
  std::vector<TyKind> kinds;
  for (ValueId v : vals) kinds.push_back(f.value(v).type.kind);
  auto s = sig.view();
  bool variadic = !s.empty() && s.back() == Arg::values;
  std::size_t fixed = s.size() - (variadic ? 1 : 0);
  if (kinds.size() < fixed || (!variadic && kinds.size() != fixed))
    return std::format("expects {} {}{}, has {}", fixed, what, variadic ? " or more" : "",
                       kinds.size());
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    Arg a = i < fixed ? s[i] : Arg::values;
    if (!matches(a, kinds[i])) return std::format("{} {} has the wrong kind", what, i);
  }
  return std::nullopt;
}

std::optional<std::string> check_imms(const Function& f, NodeId n) {
  auto words = f.imms_of(n);
  const Node& node = f.node(n);
  std::size_t at = 0;
  for (Imm k : spec_of(node.op).imm.view()) {
    if (at >= words.size()) return "too few immediates";
    std::uint64_t w = words[at++];
    auto in_range = [&](std::size_t count) { return w < count; };
    switch (k) {
      case Imm::pred: if (!in_range(enum_count<Pred>)) return "bad predicate"; break;
      case Imm::storage: if (!in_range(enum_count<Storage>)) return "bad storage"; break;
      case Imm::init: if (!in_range(enum_count<Init>)) return "bad init kind"; break;
      case Imm::order: if (!in_range(enum_count<MemOrder>)) return "bad memory order"; break;
      case Imm::rmw: if (!in_range(enum_count<Rmw>)) return "bad rmw op"; break;
      case Imm::path:
        if (w > words.size() - at) return "truncated path";
        for (std::uint64_t i = 0; i < w; ++i)
          if ((words[at + i] >> 32) >= enum_count<Step>) return "bad path step";
        at += w;
        break;
      case Imm::consts:
        if (w > words.size() - at) return "truncated constant list";
        if (node.op == Op::switch_ && w + 1 != node.nsucc)
          return "switch needs one case value per successor after the default";
        for (std::uint64_t i = 0; i < w; ++i)
          if (words[at + i] > 0xffffffffu) return "immediate out of range";
        at += w;
        break;
      case Imm::type:
      case Imm::entity:
      case Imm::constant:
      case Imm::u32:
        if (w > 0xffffffffu) return "immediate out of range";
        break;
    }
  }
  if (at != words.size()) return "too many immediates";
  return std::nullopt;
}

// Immediate dominators by the iterative algorithm of Cooper, Harvey and Kennedy ("A Simple, Fast
// Dominance Algorithm", 2001), over blocks already in reverse post-order.
std::vector<std::uint32_t> idoms(const Function& f) {
  std::size_t n = f.blocks.size();
  std::vector<std::vector<std::uint32_t>> preds(n);
  for (BlockId b : f.block_ids())
    for (const Succ& s : f.succs_of(f.terminator(b))) preds[raw(s.target)].push_back(raw(b));
  std::vector<std::uint32_t> idom(n, none);
  if (n == 0) return idom;
  idom[0] = 0;
  auto intersect = [&](std::uint32_t a, std::uint32_t b) {
    while (a != b) {
      while (a > b) a = idom[a];
      while (b > a) b = idom[b];
    }
    return a;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (std::uint32_t b = 1; b < n; ++b) {
      std::uint32_t d = none;
      for (std::uint32_t p : preds[b]) {
        if (idom[p] == none) continue;
        d = d == none ? p : intersect(p, d);
      }
      if (d != idom[b]) {
        idom[b] = d;
        changed = true;
      }
    }
  }
  return idom;
}

}  // namespace

std::expected<void, std::string> verify(const Function& f) {
  auto at = [&](NodeId n, std::string_view why) {
    return std::unexpected(std::format("node {} `{}`: {}", raw(n), info(f.node(n).op).name, why));
  };
  if (f.blocks.empty()) return std::unexpected(std::string("a function needs an entry block"));
  auto idom = idoms(f);
  auto dominates = [&](std::uint32_t a, std::uint32_t b) {
    while (b != a && b != 0 && idom[b] != none) b = idom[b];
    return a == b;
  };
  // Where each value becomes available: the block, and the node position inside it (parameters
  // before every node). A result of a terminator is available only in its normal successor.
  auto defined_before = [&](ValueId v, std::uint32_t block, std::uint32_t pos) {
    const Value& val = f.value(v);
    if (val.param) return dominates(val.def, block);
    const Node& d = f.nodes[val.def];
    if (is_terminator(d.op)) {
      if (d.nsucc == 0) return false;  // shapes are checked when its own node is visited
      std::uint32_t normal = raw(f.succs[d.succ0].target);
      return dominates(normal, block) && normal != d.block;
    }
    if (d.block == block) return val.def < pos;
    return dominates(d.block, block);
  };
  std::vector<std::uint32_t> preds(f.blocks.size());  // incoming edges
  for (const Succ& e : f.succs) ++preds[raw(e.target)];
  for (BlockId b : f.block_ids()) {
    if (raw(b) != 0 && idom[raw(b)] == none)
      return std::unexpected(std::format("block {} is unreachable", raw(b)));
    if (std::ranges::count_if(f.params_of(b), [&](ValueId p) { return f.value(p).type.kind == TyKind::state; }) > 1)
      return std::unexpected(std::format("block {} has more than one !state parameter", raw(b)));
    for (NodeId n : f.nodes_of(b)) {
      const Node& node = f.node(n);
      const OpSpec& s = spec_of(node.op);
      if (is_terminator(node.op) != (n == f.terminator(b)))
        return at(n, "a block ends with exactly one terminator");
      if (auto e = check_sig(f, s.in, f.operands_of(n), "operand")) return at(n, *e);
      if (auto e = check_sig(f, s.out, f.results_of(n), "result")) return at(n, *e);
      if (node.flags & ~s.flags) return at(n, "flag not accepted by this op");
      if (auto e = check_imms(f, n)) return at(n, *e);
      if (node.nsucc < s.succ || (!s.var_succ && node.nsucc != s.succ))
        return at(n, "wrong number of successors");
      if (s.var_succ && s.succ == 0 && node.nsucc > 1) return at(n, "at most one unwind successor");
      for (ValueId v : f.operands_of(n))
        if (!defined_before(v, raw(b), raw(n))) return at(n, std::format("%{} does not dominate its use", raw(v)));
      // A terminator's results are defined on its normal edge, so that edge must be the only way
      // into the normal successor.
      if (is_terminator(node.op) && node.nres && node.nsucc && preds[raw(f.succs_of(n)[0].target)] != 1)
        return at(n, "a terminator's results need a normal successor with no other predecessor");
      for (std::uint32_t i = 0; i < node.nsucc; ++i) {
        const Succ& e = f.succs_of(n)[i];
        if (raw(e.target) == 0) return at(n, "the entry block cannot be a branch target");
        auto implicit = implicit_params(node.op, i);
        auto params = f.params_of(e.target);
        if (params.size() != implicit.size() + e.nargs)
          return at(n, std::format("successor {} passes the wrong number of arguments", i));
        for (std::size_t k = 0; k < implicit.size(); ++k)
          if (f.value(params[k]).type.kind != implicit[k])
            return at(n, std::format("successor {} must start with the edge's implicit parameters", i));
        for (std::uint32_t k = 0; k < e.nargs; ++k) {
          ValueId a = f.args_of(e)[k];
          if (f.value(a).type != f.value(params[implicit.size() + k]).type)
            return at(n, std::format("successor {} argument {} has the wrong type", i, k));
          if (!defined_before(a, raw(b), raw(n) + 1))
            return at(n, std::format("%{} does not dominate its use", raw(a)));
        }
      }
    }
  }
  // One state chain (§2.2): on every path from a state's definition, the state is moved on (by a
  // node that yields a state, or by a terminator) at most once, and read (by a consumer that
  // yields none, as `load`) only before that. A path that comes back to the definition's block
  // meets a new instance of the state.
  struct Point {
    NodeId node;
    std::int32_t edge;  // the successor whose arguments move the state on; -1 for an operand
    bool move;
  };
  std::vector<std::vector<Point>> points(f.values.size());
  auto is_state = [&](ValueId v) { return f.value(v).type.kind == TyKind::state; };
  for (std::uint32_t i = 0; i < f.nodes.size(); ++i) {
    NodeId n{i};
    bool mover = is_terminator(f.node(n).op) || std::ranges::any_of(f.results_of(n), is_state);
    // A node consumes a state once: not twice among its operands, nor twice on one edge, nor as
    // an operand and an edge argument.
    auto ops = f.operands_of(n);
    auto twice = [&](std::span<const ValueId> a, std::span<const ValueId> b, bool same) {
      for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = same ? i + 1 : 0; j < b.size(); ++j)
          if (a[i] == b[j] && is_state(a[i])) return true;
      return false;
    };
    if (twice(ops, ops, true)) return at(n, "consumes a state twice");
    for (const Succ& e : f.succs_of(n))
      if (twice(f.args_of(e), f.args_of(e), true) || twice(ops, f.args_of(e), false))
        return at(n, "consumes a state twice");
    for (ValueId v : f.operands_of(n))
      if (is_state(v)) points[raw(v)].push_back({n, -1, mover});
    for (std::uint32_t k = 0; k < f.node(n).nsucc; ++k)
      for (ValueId v : f.args_of(f.succs_of(n)[k]))
        if (is_state(v)) points[raw(v)].push_back({n, std::int32_t(k), true});
  }
  std::vector<std::uint32_t> seen(f.blocks.size(), none);  // stamped per move
  std::uint32_t stamp = 0;
  for (std::uint32_t v = 0; v < f.values.size(); ++v) {
    if (points[v].empty()) continue;
    const Value& val = f.values[v];
    std::uint32_t def_block = val.param ? val.def : f.nodes[val.def].block;
    std::uint32_t def_entry = def_block;  // a terminator's result is defined on its normal edge
    if (!val.param && is_terminator(f.nodes[val.def].op)) def_entry = raw(f.succs[f.nodes[val.def].succ0].target);
    for (const Point& m : points[v]) {
      if (!m.move) continue;
      ++stamp;
      const Node& mn = f.node(m.node);
      auto conflict = [&](const Point& p) -> std::expected<void, std::string> {
        return at(p.node, std::format("{} %{} after node {} moved it on", p.move ? "moves" : "reads", v,
                                      raw(m.node)));
      };
      // Later in the same block.
      if (!is_terminator(mn.op))
        for (const Point& p : points[v])
          if (raw(p.node) > raw(m.node) && f.node(p.node).block == mn.block) return conflict(p);
      // Blocks reachable from here without coming back to the definition.
      std::vector<std::uint32_t> work;
      auto reach = [&](BlockId t) {
        std::uint32_t b = raw(t);
        if (b == def_block || b == def_entry || seen[b] == stamp) return;
        seen[b] = stamp;
        work.push_back(b);
      };
      if (m.edge >= 0) {
        reach(f.succs_of(m.node)[m.edge].target);
      } else {
        for (const Succ& e : f.succs_of(f.terminator(BlockId(mn.block)))) reach(e.target);
      }
      while (!work.empty()) {
        std::uint32_t b = work.back();
        work.pop_back();
        for (const Succ& e : f.succs_of(f.terminator(BlockId(b)))) reach(e.target);
      }
      for (const Point& p : points[v])
        if (seen[f.node(p.node).block] == stamp) return conflict(p);
    }
  }
  return {};
}

}  // namespace ycxx::ir
