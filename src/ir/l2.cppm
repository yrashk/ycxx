// L2, the lowered level of ycxx's IR (optimization design §2.1): flat bytes with poison. C++ is
// erased: values are `iN` or `ptr` (an i64 with provenance facts), memory is byte-addressed
// `load`/`store` of a width with an alias tag, and automatic objects are `alloca` frame objects.
// The shape is L1's: a CFG of blocks with arguments, the `!state` threaded through effectful
// operations, and the four node classes of §2.2. `lower` (lower.cppm) produces it; the optimizer
// and code generation read it.
//
// This is the α-slice subset (#26, epic #39): the ops C-like functions need. Ops are added here as
// `lower` grows; the table below is the one definition every reader uses.
//
//   func @7 {
//   ^0(%0: !state, %1: i32):
//     %2: i32 = add nsw %1, %1
//     %3: !state, %4: i32 = call %0, %2 [@12, 0, 0]
//     %5: i1 = icmp %4, %1 [slt]
//     cond_br %5 ^1(%3), ^2
//   ...
export module ycxx.ir.l2;

import std;
import ycxx.ir;

namespace ycxx::l2 {

using ir::BlockId;
using ir::EntityId;
using ir::Flag;
using ir::Flags;
using ir::NodeClass;
using ir::NodeId;
using ir::Pred;
using ir::ValueId;
using ir::raw;

export using ir::bit;
export using ir::none;

// Value types. `i1` is a truth value; `ptr` is 64 bits; `f32` and `f64` are IEEE binary32 and
// binary64 values.
export enum class Type : std::uint8_t { state, i1, i8, i16, i32, i64, ptr, f32, f64 };

export constexpr std::uint32_t bits(Type t) {
  switch (t) {
    case Type::i1: return 1;
    case Type::i8: return 8;
    case Type::i16: return 16;
    case Type::i32:
    case Type::f32: return 32;
    case Type::i64:
    case Type::ptr:
    case Type::f64: return 64;
    case Type::state: return 0;
  }
  return 0;
}
export constexpr std::uint32_t bytes(Type t) { return t == Type::i1 ? 1 : bits(t) / 8; }
export constexpr bool is_float(Type t) { return t == Type::f32 || t == Type::f64; }
export constexpr bool is_int(Type t) { return t != Type::state && t != Type::ptr && !is_float(t); }
export constexpr std::optional<Type> int_of_bits(std::uint64_t n) {
  switch (n) {
    case 1: return Type::i1;
    case 8: return Type::i8;
    case 16: return Type::i16;
    case 32: return Type::i32;
    case 64: return Type::i64;
    default: return std::nullopt;
  }
}

// Immediate operands.
export enum class Imm : std::uint8_t {
  i64,     // a constant's bits: an integer's sign-extended from its width, a float's encoding
  entity,  // a function or variable
  u32,     // a size, an alignment, an ordinal
  pred,    // a Pred
  alias,   // the alias tag: the L1 access type (a QualType), ignored by equivalence
};

export enum class Arg : std::uint8_t { state, value, values };

export template <class T, std::size_t N>
using List = ir::List<T, N>;

export struct Spec {
  NodeClass cls = NodeClass::pure;
  List<Arg, 4> in, out;
  List<Imm, 4> imm;
  std::uint8_t succ = 0;
  Flags flags = 0;

  constexpr Spec imms(List<Imm, 4> i) const { auto s = *this; s.imm = i; return s; }
  constexpr Spec accepts(std::initializer_list<Flag> fs) const {
    auto s = *this;
    for (Flag f : fs) s.flags |= bit(f);
    return s;
  }
};

namespace spec {
constexpr Arg S = Arg::state, V = Arg::value, Vs = Arg::values;
constexpr Spec make(NodeClass c, List<Arg, 4> in, List<Arg, 4> out, std::uint8_t succ = 0) {
  Spec s;
  s.cls = c;
  s.in = in;
  s.out = out;
  s.succ = succ;
  return s;
}
constexpr Spec pure(List<Arg, 4> in, List<Arg, 4> out) { return make(NodeClass::pure, in, out); }
constexpr Spec pinned(List<Arg, 4> in, List<Arg, 4> out) { return make(NodeClass::pinned, in, out); }
constexpr Spec state(List<Arg, 4> in, List<Arg, 4> out) { return make(NodeClass::state, in, out); }
constexpr Spec control(List<Arg, 4> in, std::uint8_t succ) { return make(NodeClass::control, in, {}, succ); }
using enum Imm;
using enum Flag;
}  // namespace spec

using namespace spec;

export enum class Op : std::uint16_t {
  const_ [[=pure({}, {V}).imms({i64})]],
  symbol [[=pure({}, {V}).imms({entity})]],         // the address of a function or variable
  alloca [[=pure({}, {V}).imms({u32, u32, u32})]],  // a frame object: size, alignment, ordinal

  // Integer arithmetic; `nsw`/`nuw`/`exact` make a violating result poison (§6.1).
  add [[=pure({V, V}, {V}).accepts({nsw, nuw})]],
  sub [[=pure({V, V}, {V}).accepts({nsw, nuw})]],
  mul [[=pure({V, V}, {V}).accepts({nsw, nuw})]],
  neg [[=pure({V}, {V}).accepts({nsw})]],
  sdiv [[=pinned({V, V}, {V}).accepts({exact})]],  // division by zero is immediate UB
  udiv [[=pinned({V, V}, {V}).accepts({exact})]],
  srem [[=pinned({V, V}, {V})]],
  urem [[=pinned({V, V}, {V})]],
  shl [[=pure({V, V}, {V}).accepts({nsw, nuw})]],  // an amount at or above the width is poison
  lshr [[=pure({V, V}, {V}).accepts({exact})]],
  ashr [[=pure({V, V}, {V}).accepts({exact})]],
  and_ [[=pure({V, V}, {V})]],
  or_ [[=pure({V, V}, {V})]],
  xor_ [[=pure({V, V}, {V})]],
  icmp [[=pure({V, V}, {V}).imms({pred})]],  // integers or pointers; result i1
  select [[=pure({V, V, V}, {V})]],

  // Floating point, IEEE in round-to-nearest-even. `fcmp`'s predicates are the plain ones, ordered
  // but for `ne`; the conversions to integers truncate, and a value out of range is poison.
  fadd [[=pure({V, V}, {V})]],
  fsub [[=pure({V, V}, {V})]],
  fmul [[=pure({V, V}, {V})]],
  fdiv [[=pure({V, V}, {V})]],
  fneg [[=pure({V}, {V})]],
  fabs [[=pure({V}, {V})]],
  fcmp [[=pure({V, V}, {V}).imms({pred})]],  // result i1
  fpext [[=pure({V}, {V})]],                 // f32 to f64
  fptrunc [[=pure({V}, {V})]],               // f64 to f32, rounded
  fptosi [[=pure({V}, {V})]],                // to i32 or i64
  fptoui [[=pure({V}, {V})]],
  sitofp [[=pure({V}, {V})]],                // from i32 or i64, rounded
  uitofp [[=pure({V}, {V})]],

  // Width changes, to the result type.
  zext [[=pure({V}, {V})]],
  sext [[=pure({V}, {V})]],
  trunc [[=pure({V}, {V})]],

  // Pointers.
  ptradd [[=pure({V, V}, {V})]],  // ptr + i64 bytes
  ptr2int [[=pure({V}, {V})]],    // to i64
  int2ptr [[=pure({V}, {V})]],    // from i64

  // Memory: the result's or the stored value's type is the access width.
  load [[=state({S, V}, {V}).imms({alias}).accepts({volatile_})]],
  store [[=state({S, V, V}, {S}).imms({alias}).accepts({volatile_})]],
  // Calls. The first u32 is a mask of the arguments that are signed: an argument narrower than
  // the slot the calling convention gives it is extended to it, sign-extended where its bit is
  // set. The second is how many of the last arguments the callee's ellipsis matches (L1's). The
  // third is 1 + the index of the argument that is the address of an indirect result, which the
  // calling convention passes apart (AArch64: x8), 0 if none.
  // `variadic` preserves the callee's calling convention when the tail is empty.
  call [[=state({S, Vs}, {S, Vs}).imms({entity, u32, u32, u32}).accepts({variadic})]],
  call_ind [[=state({S, V, Vs}, {S, Vs}).imms({u32, u32, u32}).accepts({variadic})]],

  // Terminators.
  br [[=control({}, 1)]],
  cond_br [[=control({V}, 2)]],  // true, false
  ret [[=control({S, Vs}, 0)]],
  unreachable [[=control({}, 0)]],

  // System V AMD64 variadic cursors. Start uses the enclosing function's entry state.
  va_start [[=state({S, V}, {S})]],
  va_arg [[=state({S, V}, {S, V})]],

  // The fixed AMD64 hosted call. It feeds the successor parameters directly, with the
  // normal result on edge 0 and the original unwind header and selector on edge 1.
  invoke [[=control({S, V}, 2).imms({entity, u32, u32, u32})]],
  // A void direct call, with one cleanup destination and no handler action.
  invoke_cleanup [[=control({S}, 2).imms({entity, u32, u32, u32})]],
};

export constexpr std::size_t op_count = std::meta::enumerators_of(^^Op).size();

export struct OpInfo {
  std::string_view name;
  Spec spec;
};

consteval std::string_view printed(std::string_view id) {
  std::string s(id);
  if (s.ends_with('_')) s.pop_back();
  return std::define_static_string(s);
}

consteval std::array<OpInfo, op_count> build_table() {
  std::array<OpInfo, op_count> t{};
  std::size_t i = 0;
  for (auto e : std::meta::enumerators_of(^^Op)) {
    auto a = std::meta::annotations_of_with_type(e, ^^Spec);
    if (a.size() != 1) throw "every Op needs exactly one Spec annotation";
    t[i].name = printed(std::meta::identifier_of(e));
    t[i].spec = std::meta::extract<Spec>(a[0]);
    if (std::to_underlying(std::meta::extract<Op>(e)) != i) throw "Op enumerators must be dense";
    ++i;
  }
  return t;
}

export constexpr std::array<OpInfo, op_count> op_table = build_table();
export constexpr const OpInfo& info(Op op) { return op_table[std::to_underlying(op)]; }
export constexpr const Spec& spec_of(Op op) { return info(op).spec; }
export constexpr bool is_terminator(Op op) { return spec_of(op).cls == NodeClass::control; }

export struct Value {
  Type type = Type::i32;
  bool param = false;
  std::uint32_t def = 0;    // the block (a parameter) or the node (a result)
  std::uint32_t index = 0;  // which parameter or result
  constexpr bool operator==(const Value&) const = default;
};

export struct Node {
  Op op{};
  Flags flags = 0;
  std::uint32_t block = 0;
  std::uint32_t res0 = 0, nres = 0;
  std::uint32_t opnd0 = 0, nopnd = 0;
  std::uint32_t imm0 = 0, nimm = 0;
  std::uint32_t succ0 = 0, nsucc = 0;
  constexpr bool operator==(const Node&) const = default;
};

// A body. Blocks keep L1's reverse post-order, so block 0 is the entry and every definition comes
// before its uses in block order. The entry block's parameters are the function's, `!state` first;
// `results` is the type list every `ret` returns.
export struct Function {
  std::vector<ir::Block> blocks;
  std::vector<Node> nodes;
  std::vector<Value> values;
  std::vector<ValueId> operands;
  std::vector<std::uint64_t> imms;
  std::vector<ir::Succ> succs;
  std::vector<Type> results;
  // 1 + the index among the entry's value parameters of the address of an indirect result, as
  // the call op's third u32; 0 if none.
  std::uint32_t indirect_result = 0;
  ir::OriginMap origins;  // node → the L1 nodes it came from

  bool operator==(const Function&) const = default;

  const ir::Block& block(BlockId b) const { return blocks[raw(b)]; }
  const Node& node(NodeId n) const { return nodes[raw(n)]; }
  const Value& value(ValueId v) const { return values[raw(v)]; }
  Type type(ValueId v) const { return value(v).type; }

  auto block_ids() const {
    return std::views::iota(0u, std::uint32_t(blocks.size())) |
           std::views::transform([](std::uint32_t i) { return BlockId(i); });
  }
  auto nodes_of(BlockId b) const {
    const ir::Block& bl = block(b);
    return std::views::iota(bl.node0, bl.node0 + bl.nnodes) |
           std::views::transform([](std::uint32_t i) { return NodeId(i); });
  }
  auto params_of(BlockId b) const {
    const ir::Block& bl = block(b);
    return std::views::iota(bl.param0, bl.param0 + bl.nparams) |
           std::views::transform([](std::uint32_t i) { return ValueId(i); });
  }
  ValueId result(NodeId n, std::uint32_t i = 0) const { return ValueId(node(n).res0 + i); }
  std::span<const ValueId> operands_of(NodeId n) const {
    return std::span(operands).subspan(node(n).opnd0, node(n).nopnd);
  }
  std::span<const std::uint64_t> imms_of(NodeId n) const {
    return std::span(imms).subspan(node(n).imm0, node(n).nimm);
  }
  std::span<const ir::Succ> succs_of(NodeId n) const {
    return std::span(succs).subspan(node(n).succ0, node(n).nsucc);
  }
  std::span<const ValueId> args_of(const ir::Succ& s) const {
    return std::span(operands).subspan(s.arg0, s.nargs);
  }
  NodeId terminator(BlockId b) const { return NodeId(block(b).node0 + block(b).nnodes - 1); }
};

// Appends blocks and nodes in order: a block's nodes are emitted before the next block starts.
// Successor arguments may name values not yet defined only through `set_arg`.
export class Builder {
 public:
  BlockId begin_block(std::span<const Type> params) {
    BlockId b{std::uint32_t(f_.blocks.size())};
    ir::Block& bl = f_.blocks.emplace_back();
    bl.param0 = std::uint32_t(f_.values.size());
    bl.nparams = std::uint32_t(params.size());
    bl.node0 = std::uint32_t(f_.nodes.size());
    for (std::uint32_t i = 0; i < params.size(); ++i) f_.values.push_back({params[i], true, raw(b), i});
    return b;
  }
  ValueId param(BlockId b, std::uint32_t i) const { return ValueId(f_.blocks[raw(b)].param0 + i); }

  struct SuccArgs {
    BlockId target{};
    std::vector<ValueId> args;
  };

  NodeId emit(Op op, std::span<const Type> results, std::span<const ValueId> operands,
              std::span<const std::uint64_t> imms = {}, Flags flags = 0, std::span<const SuccArgs> succs = {},
              std::span<const ir::Origin> origin = {}) {
    NodeId n{std::uint32_t(f_.nodes.size())};
    std::uint32_t b = std::uint32_t(f_.blocks.size() - 1);
    Node& nd = f_.nodes.emplace_back();
    nd.op = op;
    nd.flags = flags;
    nd.block = b;
    nd.res0 = std::uint32_t(f_.values.size());
    nd.nres = std::uint32_t(results.size());
    for (std::uint32_t i = 0; i < results.size(); ++i) f_.values.push_back({results[i], false, raw(n), i});
    nd.opnd0 = std::uint32_t(f_.operands.size());
    nd.nopnd = std::uint32_t(operands.size());
    f_.operands.insert(f_.operands.end(), operands.begin(), operands.end());
    nd.imm0 = std::uint32_t(f_.imms.size());
    nd.nimm = std::uint32_t(imms.size());
    f_.imms.insert(f_.imms.end(), imms.begin(), imms.end());
    nd.succ0 = std::uint32_t(f_.succs.size());
    nd.nsucc = std::uint32_t(succs.size());
    for (const SuccArgs& s : succs) {
      f_.succs.push_back({s.target, std::uint32_t(f_.operands.size()), std::uint32_t(s.args.size())});
      f_.operands.insert(f_.operands.end(), s.args.begin(), s.args.end());
    }
    f_.blocks[b].nnodes++;
    origins_.emplace_back(origin.begin(), origin.end());
    return n;
  }
  // One-result convenience.
  ValueId value(Op op, Type t, std::initializer_list<ValueId> operands, std::initializer_list<std::uint64_t> imms = {},
                Flags flags = 0, std::span<const ir::Origin> origin = {}) {
    Type ts[] = {t};
    NodeId n = emit(op, ts, std::span(operands), std::span(imms), flags, {}, origin);
    return ValueId(f_.nodes[raw(n)].res0);
  }

  void set_arg(std::uint32_t succ_index, std::uint32_t i, ValueId v) {
    f_.operands[f_.succs[succ_index].arg0 + i] = v;
  }
  const Function& peek() const { return f_; }

  Function finish(std::vector<Type> results) {
    f_.results = std::move(results);
    f_.origins = ir::OriginMap(origins_);
    return std::move(f_);
  }

 private:
  Function f_;
  std::vector<std::vector<ir::Origin>> origins_;
};

// Shapes and types: operand and result kinds per the table, equal operand and result types for
// arithmetic, `i1` conditions and compare results, widths for conversions, successor arguments
// matching the target's parameters, and `ret` matching `results`. The first problem is returned.
export std::expected<void, std::string> verify(const Function& f);

export void print(std::string& out, EntityId name, const Function& f);
export std::string print(EntityId name, const Function& f) {
  std::string s;
  print(s, name, f);
  return s;
}

export std::string_view name_of(Type t) {
  static constexpr std::string_view names[] = {"!state", "i1", "i8", "i16", "i32", "i64", "ptr", "f32", "f64"};
  return names[std::to_underlying(t)];
}

}  // namespace ycxx::l2

namespace ycxx::l2 {

namespace {

bool kind_ok(Arg a, Type t) { return a == Arg::state ? t == Type::state : t != Type::state; }

template <class R>
std::optional<std::string> check_sig(const Function& f, List<Arg, 4> sig, R&& vals, std::string_view what) {
  std::vector<Type> ts;
  for (ValueId v : vals) ts.push_back(f.type(v));
  auto s = sig.view();
  bool variadic = !s.empty() && s.back() == Arg::values;
  std::size_t fixed = s.size() - (variadic ? 1 : 0);
  if (ts.size() < fixed || (!variadic && ts.size() != fixed))
    return std::format("expects {} {}{}, has {}", fixed, what, variadic ? " or more" : "", ts.size());
  for (std::size_t i = 0; i < ts.size(); ++i)
    if (!kind_ok(i < fixed ? s[i] : Arg::values, ts[i])) return std::format("{} {} has the wrong kind", what, i);
  return std::nullopt;
}

}  // namespace

std::expected<void, std::string> verify(const Function& f) {
  if (f.blocks.empty()) return std::unexpected(std::string("a function needs an entry block"));
  std::optional<NodeId> invoke;
  for (BlockId b : f.block_ids()) {
    for (NodeId n : f.nodes_of(b)) {
      const Node& nd = f.node(n);
      const Spec& s = spec_of(nd.op);
      auto at = [&](std::string_view why) {
        return std::unexpected(std::format("node {} `{}`: {}", raw(n), info(nd.op).name, why));
      };
      if (is_terminator(nd.op) != (n == f.terminator(b))) return at("a block ends with exactly one terminator");
      std::vector<ValueId> res;
      for (std::uint32_t i = 0; i < nd.nres; ++i) res.push_back(f.result(n, i));
      if (auto e = check_sig(f, s.in, f.operands_of(n), "operand")) return at(*e);
      if (auto e = check_sig(f, s.out, res, "result")) return at(*e);
      if (nd.flags & ~s.flags) return at("flag not accepted by this op");
      if (nd.nimm != s.imm.size) return at("wrong number of immediates");
      if (nd.nsucc != s.succ) return at("wrong number of successors");
      for (ValueId v : f.operands_of(n)) {
        const Value& d = f.value(v);
        bool before = d.param ? d.def <= raw(b) : d.def < raw(n);
        if (!before) return at(std::format("%{} is used before it is defined", raw(v)));
      }
      auto ops = f.operands_of(n);
      auto t = [&](std::size_t i) { return f.type(ops[i]); };
      Type r = nd.nres ? f.type(f.result(n)) : Type::state;
      switch (nd.op) {
        case Op::add: case Op::sub: case Op::mul: case Op::sdiv: case Op::udiv: case Op::srem:
        case Op::urem: case Op::shl: case Op::lshr: case Op::ashr: case Op::and_: case Op::or_:
        case Op::xor_:
          if (!is_int(r) || t(0) != r || t(1) != r) return at("operands and result must be one integer type");
          break;
        case Op::neg:
          if (!is_int(r) || t(0) != r) return at("operand and result must be one integer type");
          break;
        case Op::fadd: case Op::fsub: case Op::fmul: case Op::fdiv:
          if (!is_float(r) || t(0) != r || t(1) != r) return at("operands and result must be one floating type");
          break;
        case Op::fneg: case Op::fabs:
          if (!is_float(r) || t(0) != r) return at("operand and result must be one floating type");
          break;
        case Op::fcmp:
          if (r != Type::i1 || !is_float(t(0)) || t(0) != t(1)) return at("compares two values of one floating type into i1");
          break;
        case Op::fpext:
          if (r != Type::f64 || t(0) != Type::f32) return at("extends an f32 to f64");
          break;
        case Op::fptrunc:
          if (r != Type::f32 || t(0) != Type::f64) return at("rounds an f64 to f32");
          break;
        case Op::fptosi: case Op::fptoui:
          if ((r != Type::i32 && r != Type::i64) || !is_float(t(0))) return at("converts a floating value to i32 or i64");
          break;
        case Op::sitofp: case Op::uitofp:
          if (!is_float(r) || (t(0) != Type::i32 && t(0) != Type::i64)) return at("converts an i32 or i64 to a floating type");
          break;
        case Op::icmp:
          if (r != Type::i1 || t(0) != t(1) || is_float(t(0))) return at("compares two integers or pointers of one type into i1");
          break;
        case Op::select:
          if (t(0) != Type::i1 || t(1) != r || t(2) != r) return at("selects between two values of the result type by an i1");
          break;
        case Op::zext: case Op::sext:
          if (!is_int(r) || !is_int(t(0)) || bits(t(0)) >= bits(r)) return at("extends to a wider integer");
          break;
        case Op::trunc:
          if (!is_int(r) || !is_int(t(0)) || bits(t(0)) <= bits(r)) return at("truncates to a narrower integer");
          break;
        case Op::ptradd:
          if (r != Type::ptr || t(0) != Type::ptr || t(1) != Type::i64) return at("adds an i64 to a ptr");
          break;
        case Op::ptr2int:
          if (r != Type::i64 || t(0) != Type::ptr) return at("converts a ptr to i64");
          break;
        case Op::int2ptr:
          if (r != Type::ptr || t(0) != Type::i64) return at("converts an i64 to ptr");
          break;
        case Op::symbol: case Op::alloca:
          if (r != Type::ptr) return at("yields a ptr");
          break;
        case Op::load: case Op::store:
          if (t(1) != Type::ptr) return at("the address must be a ptr");
          break;
        case Op::va_start: case Op::va_arg:
          if (t(1) != Type::ptr) return at("the variadic cursor must be a ptr");
          if (nd.op == Op::va_arg) {
            Type fetched = f.type(f.result(n, 1));
            if (fetched != Type::i32 && fetched != Type::i64 && fetched != Type::ptr && fetched != Type::f64)
              return at("va_arg must yield an i32, i64, ptr or f64");
          }
          break;
        case Op::cond_br:
          if (t(0) != Type::i1) return at("the condition must be i1");
          break;
        case Op::invoke:
        case Op::invoke_cleanup:
          if (invoke || (nd.op == Op::invoke && t(1) != Type::i32) ||
              f.imms_of(n)[1] > (nd.op == Op::invoke ? 1u : 0u) || f.imms_of(n)[2] || f.imms_of(n)[3] ||
              f.succs_of(n)[0].target == f.succs_of(n)[1].target)
            return at("expected one supported hosted invoke with distinct destinations");
          invoke = n;
          break;
        case Op::ret: {
          if (ops.size() - 1 != f.results.size()) return at("returns the wrong number of values");
          for (std::size_t i = 1; i < ops.size(); ++i)
            if (t(i) != f.results[i - 1]) return at("returns a value of the wrong type");
          break;
        }
        default: break;
      }
      for (std::uint32_t i = 0; i < nd.nsucc; ++i) {
        const ir::Succ& e = f.succs_of(n)[i];
        if (raw(e.target) == 0 || raw(e.target) >= f.blocks.size()) return at("bad successor");
        auto params = f.params_of(e.target);
        if (nd.op == Op::invoke || nd.op == Op::invoke_cleanup) {
          const Type normal[]{Type::state, Type::i32};
          const Type landing[]{Type::state, Type::ptr, Type::i64};
          const auto expected = i == 0 ? std::span<const Type>(normal).first(nd.op == Op::invoke ? 2 : 1)
                                       : std::span<const Type>(landing);
          if (e.nargs != 0 || !std::ranges::equal(params, expected, {}, [&](ValueId p) { return f.type(p); }))
            return at("invoke destination has the wrong implicit parameters");
          continue;
        }
        if (params.size() != e.nargs) return at(std::format("successor {} passes the wrong number of arguments", i));
        for (std::uint32_t k = 0; k < e.nargs; ++k) {
          if (f.type(f.args_of(e)[k]) != f.type(params[k]))
            return at(std::format("successor {} argument {} has the wrong type", i, k));
          const Value& d = f.value(f.args_of(e)[k]);
          if (!d.param && d.def > raw(n)) return at("a successor argument is used before it is defined");
        }
      }
    }
  }
  if (invoke) {
    for (const auto& destination : f.succs_of(*invoke))
      if (std::ranges::count(f.succs, destination.target, &ir::Succ::target) != 1)
        return std::unexpected("an invoke destination must have exactly one incoming edge");

    // A definition dominates a use when every entry path to the use goes through its block.
    // For this invoke subset, walk the graph once per defining block with uses in other blocks.
    auto reachable_without = [&](BlockId excluded) {
      std::vector<bool> seen(f.blocks.size());
      std::vector<BlockId> work{BlockId(0)};
      seen[0] = true;
      while (!work.empty()) {
        const auto b = work.back();
        work.pop_back();
        for (const auto n : f.nodes_of(b))
          for (const auto& e : f.succs_of(n))
            if (e.target != excluded && !seen[raw(e.target)]) {
              seen[raw(e.target)] = true;
              work.push_back(e.target);
            }
      }
      return seen;
    };
    const auto reachable = reachable_without(BlockId(none));
    std::vector<std::vector<BlockId>> use_blocks(f.blocks.size());
    for (const auto b : f.block_ids()) {
      if (!reachable[raw(b)]) continue;
      auto record_uses = [&](std::span<const ValueId> values) {
        for (const auto v : values) {
          const auto& value = f.value(v);
          const auto def = value.param ? value.def : f.node(NodeId(value.def)).block;
          if (def != 0 && def != raw(b)) use_blocks[def].push_back(b);
        }
      };
      for (const auto n : f.nodes_of(b)) {
        record_uses(f.operands_of(n));
        for (const auto& e : f.succs_of(n)) record_uses(f.args_of(e));
      }
    }
    for (const auto b : f.block_ids()) {
      const auto& users = use_blocks[raw(b)];
      if (users.empty()) continue;
      const auto bypass = reachable_without(b);
      if (std::ranges::any_of(users, [&](BlockId user) { return bypass[raw(user)]; }))
        return std::unexpected(std::format("a value defined in block {} does not dominate its use", raw(b)));
    }
  }
  return {};
}

void print(std::string& out, EntityId name, const Function& f) {
  auto o = std::back_inserter(out);
  std::format_to(o, "func @{} {{\n", raw(name));
  auto vals = [&](std::span<const ValueId> vs) {
    for (std::size_t i = 0; i < vs.size(); ++i) std::format_to(o, "{}%{}", i ? ", " : "", raw(vs[i]));
  };
  for (BlockId b : f.block_ids()) {
    std::format_to(o, "^{}", raw(b));
    if (f.block(b).nparams) {
      out += '(';
      bool first = true;
      for (ValueId p : f.params_of(b)) {
        std::format_to(o, "{}%{}: {}", first ? "" : ", ", raw(p), name_of(f.type(p)));
        first = false;
      }
      out += ')';
    }
    out += ":\n";
    for (NodeId n : f.nodes_of(b)) {
      const Node& nd = f.node(n);
      out += "  ";
      for (std::uint32_t i = 0; i < nd.nres; ++i)
        std::format_to(o, "{}%{}: {}", i ? ", " : "", nd.res0 + i, name_of(f.type(f.result(n, i))));
      if (nd.nres) out += " = ";
      out += info(nd.op).name;
      for (auto fl : {Flag::nsw, Flag::nuw, Flag::exact, Flag::volatile_})
        if (nd.flags & bit(fl)) std::format_to(o, " {}", ir::name_of(fl));
      if (nd.nopnd) out += ' ';
      vals(f.operands_of(n));
      auto kinds = spec_of(nd.op).imm.view();
      if (!kinds.empty()) {
        out += " [";
        auto words = f.imms_of(n);
        for (std::size_t i = 0; i < kinds.size(); ++i) {
          if (i) out += ", ";
          switch (kinds[i]) {
            case Imm::i64: std::format_to(o, "{}", std::int64_t(words[i])); break;
            case Imm::entity: std::format_to(o, "@{}", words[i]); break;
            case Imm::u32: std::format_to(o, "{}", words[i]); break;
            case Imm::pred: out += ir::name_of(Pred(words[i])); break;
            case Imm::alias: std::format_to(o, "T{}", words[i]); break;
          }
        }
        out += ']';
      }
      for (std::uint32_t i = 0; i < nd.nsucc; ++i) {
        const ir::Succ& e = f.succs_of(n)[i];
        std::format_to(o, "{}^{}", i ? ", " : " ", raw(e.target));
        if (e.nargs) {
          out += '(';
          vals(f.args_of(e));
          out += ')';
        }
      }
      out += '\n';
    }
  }
  out += "}\n";
}

}  // namespace ycxx::l2
