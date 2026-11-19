// Instruction selection, the heuristic path (optimization design §5.1; #28): greedy maximal munch
// over each block's dataflow, trees cut at shared nodes, in block order, reading the pattern table.
//
// Tree roots are the nodes whose value must exist on its own: effects and terminators, values with
// more than one use, used in another block or passed to a successor, and loads that cannot move to
// their user (a state change lies between). `const`, `symbol` and `alloca` are leaves and are
// rematerialized at every use. Every other node is folded into its user's tree: the pattern that
// covers the root may cover it too, and otherwise it is selected as its own tree just before the
// root's instructions, which is where the tree is emitted.
//
// Calls, returns and the entry block's parameters follow the calling convention (abi.cppm), not the
// table. Block arguments become copies on the edge: before the branch for an unconditional edge,
// in an edge block for a conditional one.
export module ycxx.target.aarch64:select;

import std;
import ycxx.ir;
import ycxx.ir.l2;
import :isa;
import :abi;
import :mir;
import :patterns;

namespace ycxx::aarch64 {

using ir::EntityId;
using ir::NodeId;
using ir::ValueId;
using ir::BlockId;
using ir::raw;
constexpr std::uint32_t nil = ir::none;
using l2::Type;

// `defined` lists the entities this unit defines, sorted: their addresses are formed directly,
// every other entity's through the GOT.
export std::expected<MFunction, std::string> select(const l2::Function& f, std::span<const EntityId> defined,
                                                    const CallingConvention& cc = apple_arm64,
                                                    const Table& table = patterns());

// The condition a predicate holds under after a compare. The plain predicates are `fcmp`'s
// (integers and pointers use the signed and unsigned ones): FCMP sets NZCV to 0011 for unordered
// operands, so `lt` is MI and `le` LS, which unordered fails, as do GT and GE; `ne` holds then.
export constexpr Cond cond_of(ir::Pred p) {
  using enum ir::Pred;
  switch (p) {
    case eq: return Cond::eq;
    case ne: return Cond::ne;
    case slt: return Cond::lt;
    case lt: return Cond::mi;
    case sle: return Cond::le;
    case le: return Cond::ls;
    case sgt: case gt: return Cond::gt;
    case sge: case ge: return Cond::ge;
    case ult: return Cond::lo;
    case ule: return Cond::ls;
    case ugt: return Cond::hi;
    case uge: return Cond::hs;
  }
  return Cond::al;
}

constexpr std::uint8_t log2_bytes(Type t) {
  switch (l2::bytes(t)) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    default: return 3;
  }
}

class Selector {
 public:
  Selector(const l2::Function& f, std::span<const EntityId> defined, const CallingConvention& cc, const Table& t)
      : f_(f), defined_(defined), cc_(cc), t_(t) {}

  std::expected<MFunction, std::string> run() {
    analyse();
    m_.labels = std::uint32_t(f_.blocks.size());
    for (BlockId b : f_.block_ids())
      for (ValueId p : f_.params_of(b))
        if (f_.type(p) != Type::state) vreg_of_[raw(p)] = new_vreg(f_.type(p));
    for (BlockId b : f_.block_ids()) {
      MBlock& mb = m_.blocks.emplace_back();
      mb.label = raw(b);
      out_ = &m_.blocks.back().insts;
      cur_ = raw(b);
      if (raw(b) == 0) params();
      for (NodeId n : f_.nodes_of(b)) {
        if (!root_[raw(n)]) continue;
        origin_ = raw(n);
        root(n);
        if (!error_.empty()) return std::unexpected(error_);
      }
    }
    for (MBlock& e : edges_) m_.blocks.push_back(std::move(e));
    return std::move(m_);
  }

 private:
  struct Bind {
    enum class K : std::uint8_t { unbound, value, imm, entity, frame, pred } k = K::unbound;
    std::int64_t v = 0;
    constexpr bool operator==(const Bind&) const = default;
  };
  using Binds = std::array<Bind, 16>;

  const l2::Function& f_;
  std::span<const EntityId> defined_;
  const CallingConvention& cc_;
  const Table& t_;
  MFunction m_;
  std::vector<std::uint32_t> vreg_of_, uses_, user_, frame_of_;
  std::vector<std::uint8_t> succ_use_, root_;
  std::vector<MInst>* out_ = nullptr;
  std::vector<MBlock> edges_;
  std::uint32_t cur_ = 0, origin_ = nil;
  std::string error_;

  static bool leaf(l2::Op op) { return op == l2::Op::const_ || op == l2::Op::symbol || op == l2::Op::alloca; }
  bool is_local(EntityId e) const { return std::ranges::binary_search(defined_, raw(e), {}, [](EntityId x) { return raw(x); }); }

  std::uint32_t new_vreg(Type t) {
    m_.vregs.push_back(t);
    return std::uint32_t(m_.vregs.size() - 1);
  }
  void fail(std::string why) {
    if (error_.empty()) error_ = std::format("node {}: {}", origin_, std::move(why));
  }

  void analyse() {
    std::size_t nv = f_.values.size(), nn = f_.nodes.size();
    vreg_of_.assign(nv, nil);
    uses_.assign(nv, 0);
    user_.assign(nv, nil);
    succ_use_.assign(nv, 0);
    root_.assign(nn, 0);
    frame_of_.assign(nn, nil);
    for (std::uint32_t i = 0; i < nn; ++i) {
      NodeId n{i};
      for (ValueId v : f_.operands_of(n)) {
        ++uses_[raw(v)];
        user_[raw(v)] = i;
      }
      for (const ir::Succ& s : f_.succs_of(n))
        for (ValueId v : f_.args_of(s)) {
          ++uses_[raw(v)];
          succ_use_[raw(v)] = 1;
        }
      if (f_.node(n).op == l2::Op::alloca) {
        auto im = f_.imms_of(n);
        frame_of_[i] = std::uint32_t(m_.objects.size());
        m_.objects.push_back({im[0], std::uint32_t(im[1])});
      }
    }
    // Epochs: the number of state changes before a node in its block. A load moves to its user's
    // emission point only within one epoch.
    std::vector<std::uint32_t> epoch(nn), emit_at(nn);
    for (BlockId b : f_.block_ids()) {
      std::uint32_t e = 0;
      for (NodeId n : f_.nodes_of(b)) {
        epoch[raw(n)] = e;
        const l2::Node& nd = f_.node(n);
        for (std::uint32_t r = 0; r < nd.nres; ++r)
          if (f_.type(f_.result(n, r)) == Type::state) ++e;
      }
      auto ids = f_.nodes_of(b);
      for (auto it = ids.end(); it != ids.begin();) {
        NodeId n = *--it;
        std::uint32_t i = raw(n);
        const l2::Node& nd = f_.node(n);
        emit_at[i] = i;
        if (leaf(nd.op)) continue;
        auto cls = l2::spec_of(nd.op).cls;
        bool is_load = nd.op == l2::Op::load;
        if ((cls == ir::NodeClass::state && !is_load) || cls == ir::NodeClass::control) {
          root_[i] = 1;
          continue;
        }
        ValueId v = f_.result(n);
        std::uint32_t u = user_[raw(v)];
        if (uses_[raw(v)] == 0) {  // dead: a volatile load still happens
          root_[i] = is_load && (nd.flags & l2::bit(ir::Flag::volatile_));
          continue;
        }
        bool fold = uses_[raw(v)] == 1 && !succ_use_[raw(v)] && u != nil && f_.node(NodeId(u)).block == nd.block;
        if (fold && is_load) fold = epoch[i] == epoch[emit_at[u]] && !(nd.flags & l2::bit(ir::Flag::volatile_));
        root_[i] = !fold;
        if (fold) emit_at[i] = emit_at[u];
      }
    }
  }

  void emit(MInst in) {
    in.origin = origin_;
    out_->push_back(in);
  }
  MInst mov(MOp d, MOp s) {
    MInst in{Form::MOVrr, true, 3, 2, {d, s}};
    return in;
  }

  // The value in a virtual register, selecting its tree here if it has nil yet.
  std::uint32_t vreg(ValueId v) {
    if (vreg_of_[raw(v)] != nil) return vreg_of_[raw(v)];
    const l2::Value& val = f_.value(v);
    NodeId n{val.def};
    std::uint32_t d = new_vreg(val.type);
    munch(n, d);
    if (!leaf(f_.node(n).op)) vreg_of_[raw(v)] = d;
    return d;
  }

  // Where the i-th stack argument of `type` goes, from the start of the argument area.
  std::uint32_t stack_slot(std::uint32_t& at, Type type) const {
    std::uint32_t size = cc_.natural_stack_args ? l2::bytes(type) : 8;
    at = (at + size - 1) / size * size;
    std::uint32_t here = at;
    at += size;
    return here;
  }

  // The argument registers of `t`'s class, and the results'.
  std::span<const R> arg_regs(Type t) const { return l2::is_float(t) ? std::span<const R>(cc_.fp_args) : cc_.int_args; }
  std::span<const R> result_regs(Type t) const {
    return l2::is_float(t) ? std::span<const R>(cc_.fp_results) : std::span<const R>(cc_.int_results).first(cc_.max_results);
  }

  // Integer and floating arguments take the next register of their class, then stack slots in
  // argument order; once a class's registers are used up, its later arguments go on the stack
  // too (AAPCS64 C.3, C.11).
  void params() {
    std::uint32_t stack = 0, k = 0;
    std::array<std::uint32_t, 2> next{};  // integer, floating
    for (ValueId p : f_.params_of(BlockId(0))) {
      Type t = f_.type(p);
      if (t == Type::state) continue;
      std::uint32_t v = vreg_of_[raw(p)];
      if (++k == f_.indirect_result) {  // the address of the indirect result comes apart
        emit(mov(MOp::vreg(v), MOp::preg(cc_.indirect_result)));
        continue;
      }
      std::uint32_t& i = next[l2::is_float(t)];
      if (i < arg_regs(t).size()) {
        emit(mov(MOp::vreg(v), MOp::preg(arg_regs(t)[i])));
      } else {
        std::uint32_t at = stack_slot(stack, t);
        emit({Form::LDRui, true, log2_bytes(t), 3, {MOp::vreg(v), MOp::preg(R::fp), MOp::imm(cc_.frame_record + at)}});
      }
      ++i;
    }
  }

  void root(NodeId n) {
    const l2::Node& nd = f_.node(n);
    switch (nd.op) {
      case l2::Op::call:
      case l2::Op::call_ind: call(n); return;
      case l2::Op::ret: ret(n); return;
      default: break;
    }
    std::uint32_t d = nil;
    for (std::uint32_t r = 0; r < nd.nres; ++r)
      if (f_.type(f_.result(n, r)) != Type::state) {
        d = new_vreg(f_.type(f_.result(n, r)));
        vreg_of_[raw(f_.result(n, r))] = d;
      }
    munch(n, d);
  }

  // `v` of type `t` extended to `bits`, by its signedness `s`; `v` itself when it is that wide.
  std::uint32_t extend(std::uint32_t v, Type t, std::uint32_t bits, bool s) {
    if (l2::bits(t) >= bits) return v;
    std::uint32_t x = new_vreg(*l2::int_of_bits(bits));
    emit({s ? Form::SBFM : Form::UBFM, bits == 64, 3, 4, {MOp::vreg(x), MOp::vreg(v), MOp::imm(0), MOp::imm(l2::bits(t) - 1)}});
    return x;
  }

  void call(NodeId n) {
    const l2::Node& nd = f_.node(n);
    auto ops = f_.operands_of(n);
    auto imms = f_.imms_of(n);
    bool ind = nd.op == l2::Op::call_ind;
    std::uint32_t target = ind ? vreg(ops[1]) : nil;
    std::uint64_t sext = 0, sext_l2 = imms[ind ? 0 : 1];
    std::uint64_t indirect = imms[ind ? 2 : 3];
    std::uint32_t indirect_reg = nil;
    std::vector<std::pair<std::uint32_t, Type>> args;
    for (std::size_t i = ind ? 2 : 1, k = 0; i < ops.size(); ++i, ++k) {
      if (k + 1 == indirect) {  // the indirect result's address goes apart (AAPCS64 §6.9: x8)
        indirect_reg = vreg(ops[i]);
        continue;
      }
      if ((sext_l2 >> k) & 1) sext |= std::uint64_t(1) << args.size();
      args.push_back({vreg(ops[i]), f_.type(ops[i])});
    }
    // The arguments the callee's ellipsis matches are the last ones; the convention may place
    // them apart from the named ones.
    std::size_t through_ellipsis = imms[ind ? 1 : 2];
    if (through_ellipsis > args.size()) return fail("more arguments matching the ellipsis than arguments");
    std::size_t named = cc_.variadic_slot ? args.size() - through_ellipsis : args.size();
    auto is_signed = [&](std::size_t i) { return i < 64 && ((sext >> i) & 1); };
    std::uint32_t stack = 0;
    std::array<std::uint32_t, 2> next{};  // integer, floating registers taken, as in `params`
    std::vector<std::pair<R, std::uint32_t>> in_regs;
    for (std::size_t i = 0; i < named; ++i) {
      std::uint32_t& r = next[l2::is_float(args[i].second)];
      if (r < arg_regs(args[i].second).size()) {
        // Extended to the register width by signedness.
        auto [v, t] = args[i];
        in_regs.push_back({arg_regs(t)[r++], l2::is_float(t) ? v : extend(v, t, cc_.reg_arg_bits, is_signed(i))});
        continue;
      }
      ++r;
      std::uint32_t at = stack_slot(stack, args[i].second);
      std::uint32_t v = args[i].first;
      if (args[i].second == Type::i1) {  // a bool's byte is 0 or 1; bits above bit 0 are unspecified
        std::uint32_t x = new_vreg(Type::i8);
        emit({Form::UBFM, false, 3, 4, {MOp::vreg(x), MOp::vreg(v), MOp::imm(0), MOp::imm(0)}});
        v = x;
      }
      emit({Form::STRui, true, log2_bytes(args[i].second), 3, {MOp::vreg(v), MOp::preg(R::sp), MOp::imm(at)}});
    }
    for (std::size_t i = named; i < args.size(); ++i) {  // each in a slot of its own
      std::uint32_t size = cc_.variadic_slot;
      stack = (stack + size - 1) / size * size;
      std::uint32_t v = extend(args[i].first, args[i].second, size * 8, is_signed(i));
      emit({Form::STRui, true, log2_bytes(*l2::int_of_bits(size * 8)), 3, {MOp::vreg(v), MOp::preg(R::sp), MOp::imm(stack)}});
      stack += size;
    }
    m_.outgoing = std::max(m_.outgoing, (stack + cc_.stack_align - 1) / cc_.stack_align * cc_.stack_align);
    for (auto [r, v] : in_regs) emit(mov(MOp::preg(r), MOp::vreg(v)));
    if (indirect_reg != nil) emit(mov(MOp::preg(cc_.indirect_result), MOp::vreg(indirect_reg)));
    if (ind) emit({Form::BLR, true, 3, 1, {MOp::vreg(target)}});
    else emit({Form::BL, true, 3, 1, {{MK::sym, std::int64_t(f_.imms_of(n)[0])}}});
    std::array<std::uint32_t, 2> taken{};
    for (std::uint32_t r = 1; r < nd.nres; ++r) {
      ValueId v = f_.result(n, r);
      Type t = f_.type(v);
      std::uint32_t& i = taken[l2::is_float(t)];
      if (i >= result_regs(t).size()) return fail("more results than the calling convention returns in registers");
      std::uint32_t d = new_vreg(t);
      vreg_of_[raw(v)] = d;
      emit(mov(MOp::vreg(d), MOp::preg(result_regs(t)[i++])));
    }
  }

  void ret(NodeId n) {
    auto ops = f_.operands_of(n);
    std::array<std::uint32_t, 2> taken{};
    for (std::size_t r = 1; r < ops.size(); ++r) {
      Type t = f_.type(ops[r]);
      std::uint32_t& i = taken[l2::is_float(t)];
      if (i >= result_regs(t).size()) return fail("more results than the calling convention returns in registers");
      emit(mov(MOp::preg(result_regs(t)[i++]), MOp::vreg(vreg(ops[r]))));
    }
    emit({Form::RETp, true, 3, 0, {}});
  }

  // --- Matching --------------------------------------------------------------------------------

  Bind imm_of(NodeId n) const {
    const l2::Node& nd = f_.node(n);
    auto im = f_.imms_of(n);
    switch (nd.op) {
      case l2::Op::const_: return {Bind::K::imm, std::int64_t(im[0])};
      case l2::Op::symbol: return {Bind::K::entity, std::int64_t(im[0])};
      case l2::Op::alloca: return {Bind::K::frame, frame_of_[raw(n)]};
      case l2::Op::icmp:
      case l2::Op::fcmp: return {Bind::K::pred, std::int64_t(im[0])};
      default: return {};
    }
  }

  bool bind(Binds& b, std::uint8_t var, Bind v) const {
    if (b[var].k == Bind::K::unbound) {
      b[var] = v;
      return true;
    }
    return b[var] == v;
  }

  bool match_value(std::uint16_t pn, ValueId v, Binds& b) const {
    const PNode& p = t_.nodes[pn];
    if (p.is_var) return bind(b, p.var, {Bind::K::value, raw(v)});
    const l2::Value& val = f_.value(v);
    if (val.param) return false;
    NodeId n{val.def};
    const l2::Node& nd = f_.node(n);
    if (nd.op != p.op || (!leaf(nd.op) && root_[raw(n)])) return false;
    return match_node(pn, n, b);
  }

  bool match_node(std::uint16_t pn, NodeId n, Binds& b) const {
    const PNode& p = t_.nodes[pn];
    if (f_.node(n).op != p.op) return false;
    if (p.imm == PNode::ImmKind::var && !bind(b, p.var, imm_of(n))) return false;
    if (p.imm == PNode::ImmKind::lit && std::int64_t(f_.imms_of(n)[0]) != p.lit) return false;
    std::uint16_t k = 0;
    for (ValueId v : f_.operands_of(n)) {
      if (f_.type(v) == Type::state) continue;
      if (k >= p.count || !match_value(t_.children[p.first + k], v, b)) return false;
      ++k;
    }
    return k == p.count;
  }

  // The access size of a load or store root.
  std::uint32_t access_bytes(NodeId n) const {
    const l2::Node& nd = f_.node(n);
    if (nd.op == l2::Op::load) return l2::bytes(f_.type(f_.result(n)));
    if (nd.op == l2::Op::store) return l2::bytes(f_.type(f_.operands_of(n)[2]));
    return 1;
  }

  bool holds(const PCond& c, const Binds& b, NodeId root) const {
    if (c.var == root_var) {  // ?d: the root's result
      Type t = f_.type(f_.result(root));
      return c.kind == CKind::f32 ? t == Type::f32 : c.kind == CKind::f64 ? t == Type::f64 : false;
    }
    const Bind& x = b[c.var];
    std::int64_t v = x.v;
    auto type = [&] { return f_.type(ValueId(std::uint32_t(v))); };
    switch (c.kind) {
      case CKind::f32: return x.k == Bind::K::value && type() == Type::f32;
      case CKind::f64: return x.k == Bind::K::value && type() == Type::f64;
      case CKind::uimm12: return x.k == Bind::K::imm && v >= 0 && v < 4096;
      case CKind::nuimm12: return x.k == Bind::K::imm && v < 0 && v > -4096;
      case CKind::nonneg: return x.k == Bind::K::imm && v >= 0 && v < (std::int64_t(1) << 24);
      case CKind::uoff: {
        std::int64_t s = access_bytes(root);
        return x.k == Bind::K::imm && v >= 0 && v % s == 0 && v / s < 4096;
      }
      case CKind::local: return x.k == Bind::K::entity && is_local(EntityId(std::uint32_t(v)));
      case CKind::extern_: return x.k == Bind::K::entity && !is_local(EntityId(std::uint32_t(v)));
      case CKind::wide: return x.k == Bind::K::value && l2::bits(type()) >= 32;
      case CKind::narrow: return x.k == Bind::K::value && l2::bits(type()) < 32;
      case CKind::signed_:
      case CKind::unsigned_: {
        if (x.k != Bind::K::pred) return false;
        using enum ir::Pred;
        auto p = ir::Pred(v);
        bool s = p == slt || p == sle || p == sgt || p == sge || p == lt || p == le || p == gt || p == ge;
        return s == (c.kind == CKind::signed_);
      }
    }
    return false;
  }

  // Selects the tree rooted at `n` into `d` (nil for a node without a value).
  void munch(NodeId n, std::uint32_t d) {
    const l2::Node& nd = f_.node(n);
    const Pattern* best = nullptr;
    Binds best_b{};
    for (const Pattern& p : t_.patterns) {
      if (t_.nodes[p.root].op != nd.op) continue;
      Binds b{};
      if (!match_node(p.root, n, b)) continue;
      bool ok = true;
      for (std::uint16_t i = 0; i < p.nconds && ok; ++i) ok = holds(t_.conds[p.first_cond + i], b, n);
      if (!ok) continue;
      if (best && (p.size < best->size || (p.size == best->size && p.cost >= best->cost))) continue;
      best = &p;
      best_b = b;
    }
    if (!best) return fail(std::format("no pattern matches `{}`", l2::info(nd.op).name));
    instantiate(*best, best_b, d);
  }

  void instantiate(const Pattern& p, const Binds& b, std::uint32_t d) {
    std::array<std::uint32_t, 16> regs{};
    for (std::uint8_t i = 0; i < p.nvars; ++i)
      if (b[i].k == Bind::K::value) regs[i] = vreg(ValueId(std::uint32_t(b[i].v)));
    // Temporaries hold integers or addresses: of the root's type, unless that is floating.
    Type temp_type = d != nil && !l2::is_float(m_.vregs[d]) ? m_.vregs[d] : Type::i64;
    std::array<std::uint32_t, 4> temps;
    temps.fill(nil);
    for (std::uint16_t k = 0; k < p.ninsts; ++k) {
      const RInst& r = t_.insts[p.first_inst + k];
      const FormSpec& fs = spec_of(r.form);
      MInst in{r.form, false, 3, std::uint8_t(r.ops.size), {}};
      std::optional<std::uint32_t> succ;
      // The width: of the integer registers, or of the floating ones when there are none (a
      // conversion's floating operand has its precision in the form).
      bool any_int = false, int_wide = false, fp_wide = false;
      for (std::uint8_t i = 0; i < r.ops.size; ++i) {
        const ROp& o = r.ops.at[i];
        Role role = fs.ops.at[i].role;
        MOp& m = in.ops[i];
        switch (o.kind) {
          case RKind::var: {
            const Bind& x = b[o.v];
            switch (x.k) {
              case Bind::K::value: m = MOp::vreg(regs[o.v]); break;
              case Bind::K::imm: m = MOp::imm(x.v); break;
              case Bind::K::entity: m = {MK::sym, x.v}; break;
              case Bind::K::frame: m = {MK::frame, x.v}; break;
              case Bind::K::pred: m = {MK::cond, std::to_underlying(cond_of(ir::Pred(x.v)))}; break;
              case Bind::K::unbound: return fail(std::format("pattern {}: unbound variable", p.name));
            }
            break;
          }
          case RKind::dst:
            if (d == nil) return fail(std::format("pattern {}: ?d of a node without a value", p.name));
            m = MOp::vreg(d);
            break;
          case RKind::temp:
            if (temps[o.v] == nil) temps[o.v] = new_vreg(temp_type);
            m = MOp::vreg(temps[o.v]);
            break;
          case RKind::lit: m = MOp::imm(o.v); break;
          case RKind::neg: m = MOp::imm(-b[o.v].v); break;
          case RKind::msb: m = MOp::imm(l2::bits(f_.type(ValueId(std::uint32_t(b[o.v].v)))) - 1); break;
          case RKind::zr: m = MOp::preg(R::zr); break;
          case RKind::sp: m = MOp::preg(R::sp); break;
          case RKind::cond: m = {MK::cond, o.v}; break;
          case RKind::succ: succ = std::uint32_t(o.v); break;
        }
        bool reg_role = role == Role::def || role == Role::use;
        if (reg_role != (m.kind == MK::vreg || m.kind == MK::preg) && o.kind != RKind::succ)
          return fail(std::format("pattern {}: operand {} of {} has the wrong kind", p.name, i, info(r.form).name));
        if (m.kind == MK::vreg) {
          Type t = m_.vregs[m.v];
          bool w = l2::bits(t) == 64;
          if (l2::is_float(t)) fp_wide |= w;
          else any_int = true, int_wide |= w;
        }
      }
      in.wide = any_int ? int_wide : fp_wide;
      if (d != nil && r.form == Form::MOVi) in.wide = l2::bits(m_.vregs[d]) == 64;
      if (fs.size == Size::mem || r.form == Form::LDRfi || r.form == Form::STRfi)
        in.access = log2_bytes(m_.vregs[in.ops[0].v]);
      if (succ) {
        NodeId term = f_.terminator(BlockId(cur_));
        const ir::Succ& s = f_.succs_of(term)[*succ];
        if (r.form == Form::B) {
          copies(s);
          if (raw(s.target) == cur_ + 1) continue;  // falls through
          in.ops[0] = {MK::label, raw(s.target)};
        } else {
          for (std::uint8_t i = 0; i < r.ops.size; ++i)
            if (r.ops.at[i].kind == RKind::succ) in.ops[i] = {MK::label, edge_label(s)};
        }
      }
      emit(in);
    }
  }

  // --- Edges -----------------------------------------------------------------------------------

  // The parallel copy of a successor's arguments into its parameters' registers. When a source is
  // also a destination, every source goes through a temporary first.
  void copies(const ir::Succ& s) {
    auto args = f_.args_of(s);
    auto params = f_.params_of(s.target);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> moves;  // (dst, src)
    std::uint32_t i = 0;
    for (ValueId p : params) {
      ValueId a = args[i++];
      if (f_.type(p) == Type::state) continue;
      std::uint32_t src = vreg(a), dst = vreg_of_[raw(p)];
      if (src != dst) moves.push_back({dst, src});
    }
    bool overlap = std::ranges::any_of(moves, [&](auto m) {
      return std::ranges::any_of(moves, [&](auto n) { return n.first == m.second; });
    });
    if (overlap)
      for (auto& [dst, src] : moves) {
        std::uint32_t t = new_vreg(m_.vregs[src]);
        emit(mov(MOp::vreg(t), MOp::vreg(src)));
        src = t;
      }
    for (auto [dst, src] : moves) emit(mov(MOp::vreg(dst), MOp::vreg(src)));
  }

  // Where a conditional branch along `s` goes: the target, or an edge block holding the copies.
  std::uint32_t edge_label(const ir::Succ& s) {
    bool any = false;
    for (std::uint32_t i = 0; i < s.nargs; ++i) any |= f_.type(f_.args_of(s)[i]) != Type::state;
    if (!any) return raw(s.target);
    MBlock e;
    e.label = m_.labels++;
    std::vector<MInst>* saved = out_;
    out_ = &e.insts;
    copies(s);
    emit({Form::B, true, 3, 1, {{MK::label, raw(s.target)}}});
    out_ = saved;
    edges_.push_back(std::move(e));
    return edges_.back().label;
  }
};

std::expected<MFunction, std::string> select(const l2::Function& f, std::span<const EntityId> defined,
                                             const CallingConvention& cc, const Table& table) {
  return Selector(f, defined, cc, table).run();
}

}  // namespace ycxx::aarch64
