// The interpreter's evaluator: one handler per L1 op, named after it, gathered into the dispatch
// table by `ir::eval_table` (ycxx.ir:ops) with `complete`, so a new op does not compile until it
// has a handler here. An op the α slice does not evaluate yet fails with `Reason::unsupported`.
module ycxx.interp;

import std;
import ycxx.ast;
import ycxx.ir;

namespace ycxx::interp {

using ir::Op;
using M = Machine;
using N = NodeId;

struct Ops {
  using Flow = M::Flow;
  using Frame = M::Frame;
  using Handler = Flow(M&, Frame&, N);

  // ---------------------------------------------------------------- helpers

  static std::span<const u64> imms(Frame& f, N n) { return f.fn->imms_of(n); }
  static QualType imm_type(Frame& f, N n, u32 i = 0) { return QualType(u32(imms(f, n)[i])); }
  // The access path immediate starting at word `at`: its steps.
  static std::span<const u64> imm_path(Frame& f, N n, u32 at) {
    auto w = imms(f, n);
    return w.subspan(at + 1, w[at]);
  }
  static bool has(Frame& f, N n, ir::Flag fl) { return f.fn->node(n).flags & ir::bit(fl); }
  static bool constant(M& m) { return m.config_.mode == Mode::constant; }

  // ---------------------------------------------------------------- constants and addresses

  static Flow const_(M& m, Frame& f, N n) {
    Value v;
    if (Flow fl = m.constant(ConstId(u32(imms(f, n)[0])), m.result_type(f, n), v); fl != Flow::next) return fl;
    return m.set(f, n, v);
  }

  static Flow global(M& m, Frame& f, N n) {
    u32 o = 0;
    if (Flow fl = m.global_object(EntityId(u32(imms(f, n)[0])), o); fl != Flow::next) return fl;
    return m.set(f, n, Value{VKind::object, false, o, 0});
  }

  static Flow func_addr(M& m, Frame& f, N n) { return m.set(f, n, Value::function(EntityId(u32(imms(f, n)[0])))); }
  static Flow vtable_addr(M& m, Frame&, N n) { return m.unsupported(n); }

  // ---------------------------------------------------------------- integers

  static Flow add(M& m, Frame& f, N n) { return m.arith(f, n); }
  static Flow sub(M& m, Frame& f, N n) { return m.arith(f, n); }
  static Flow mul(M& m, Frame& f, N n) { return m.arith(f, n); }
  static Flow neg(M& m, Frame& f, N n) { return m.arith(f, n); }
  static Flow and_(M& m, Frame& f, N n) { return m.arith(f, n); }
  static Flow or_(M& m, Frame& f, N n) { return m.arith(f, n); }
  static Flow xor_(M& m, Frame& f, N n) { return m.arith(f, n); }
  static Flow sdiv(M& m, Frame& f, N n) { return m.divide(f, n); }
  static Flow udiv(M& m, Frame& f, N n) { return m.divide(f, n); }
  static Flow srem(M& m, Frame& f, N n) { return m.divide(f, n); }
  static Flow urem(M& m, Frame& f, N n) { return m.divide(f, n); }
  static Flow shl(M& m, Frame& f, N n) { return m.shift(f, n); }
  static Flow lshr(M& m, Frame& f, N n) { return m.shift(f, n); }
  static Flow ashr(M& m, Frame& f, N n) { return m.shift(f, n); }

  static Flow icmp(M& m, Frame& f, N n) {
    Value a = m.operand(f, n, 0), b = m.operand(f, n, 1);
    if (a.kind != VKind::integer || b.kind != VKind::integer) return m.fail(Reason::ill_formed);
    i64 sa = i64(a.bits), sb = i64(b.bits);
    u64 ua = a.bits, ub = b.bits;
    bool r;
    switch (ir::Pred(imms(f, n)[0])) {
      using enum ir::Pred;
    case eq: r = ua == ub; break;
    case ne: r = ua != ub; break;
    case slt: r = sa < sb; break;
    case sle: r = sa <= sb; break;
    case sgt: r = sa > sb; break;
    case sge: r = sa >= sb; break;
    case ult: r = ua < ub; break;
    case ule: r = ua <= ub; break;
    case ugt: r = ua > ub; break;
    case uge: r = ua >= ub; break;
    default: return m.fail(Reason::ill_formed);
    }
    return m.set(f, n, Value::integer(r));
  }

  static Flow select(M& m, Frame& f, N n) {
    Value c = m.operand(f, n, 0);
    if (c.kind != VKind::integer) return m.fail(Reason::ill_formed);
    return m.set(f, n, m.operand(f, n, c.bits ? 1 : 2));
  }

  // ---------------------------------------------------------------- floating point

  static Flow fadd(M& m, Frame& f, N n) { return m.float_arith(f, n); }
  static Flow fsub(M& m, Frame& f, N n) { return m.float_arith(f, n); }
  static Flow fmul(M& m, Frame& f, N n) { return m.float_arith(f, n); }
  static Flow fdiv(M& m, Frame& f, N n) { return m.float_arith(f, n); }
  static Flow fneg(M& m, Frame& f, N n) { return m.float_arith(f, n); }
  static Flow fabs(M& m, Frame& f, N n) { return m.float_arith(f, n); }
  static Flow fcmp(M& m, Frame& f, N n) { return m.float_compare(f, n); }

  // ---------------------------------------------------------------- conversions

  // [conv.integral], [conv.prom]: the value modulo 2^N of the result type.
  static Flow iconv(M& m, Frame& f, N n) {
    Value a = m.operand(f, n, 0);
    const M::Shape* s;
    if (Flow fl = m.int_shape(f, m.result_type(f, n), s); fl != Flow::next) return fl;
    if (a.kind != VKind::integer) return m.fail(Reason::ill_formed);
    // To bool it is the boolean conversion ([conv.integral]/1 defers to [conv.bool]).
    if (ast::builtin_of(*m.in_, m.result_type(f, n)) == ast::Builtin::bool_) return m.set(f, n, Value::integer(a.bits != 0));
    return m.set(f, n, Value::integer(M::norm(a.bits, s->width, s->is_signed)));
  }
  static Flow fconv(M& m, Frame& f, N n) { return m.float_convert(f, n); }
  static Flow f2i(M& m, Frame& f, N n) { return m.float_convert(f, n); }
  static Flow i2f(M& m, Frame& f, N n) { return m.float_convert(f, n); }
  static Flow bit_cast(M& m, Frame&, N n) { return m.unsupported(n); }

  // [conv.bool]: zero and the null pointer are false; a floating zero of either sign is, and a NaN
  // is not. A pointer to an object or a function is never null.
  static Flow tobool(M& m, Frame& f, N n) {
    Value a = m.operand(f, n, 0);
    switch (a.kind) {
    case VKind::integer: return m.set(f, n, Value::integer(a.bits != 0));
    case VKind::floating: {
      u64 magnitude = m.shape(m.type_of(f, f.fn->operands_of(n)[0])).width == 32 ? a.bits & 0x7fff'ffffu : a.bits & ~(u64{1} << 63);
      return m.set(f, n, Value::integer(magnitude != 0));
    }
    case VKind::null: return m.set(f, n, Value::integer(0));
    case VKind::object:
    case VKind::function: return m.set(f, n, Value::integer(1));
    case VKind::none: break;
    }
    return m.fail(Reason::ill_formed);
  }

  // ---------------------------------------------------------------- pointers

  // [expr.add]/4: the result must point into the array, or one past its end; null plus zero is
  // null.
  static Flow ptr_add(M& m, Frame& f, N n) {
    Value p = m.operand(f, n, 0), i = m.operand(f, n, 1);
    const M::Shape* is;
    if (Flow fl = m.int_shape(f, m.type_of(f, f.fn->operands_of(n)[1]), is); fl != Flow::next) return fl;
    if (i.kind != VKind::integer) return m.fail(Reason::ill_formed);
    bool negative = is->is_signed && i64(i.bits) < 0;
    if (!negative && i.bits > u64(std::numeric_limits<i64>::max())) return m.fail(Reason::pointer_bounds);
    i64 by = i64(i.bits);
    if (p.kind == VKind::null) {
      if (by != 0) return m.fail(Reason::pointer_bounds);
      return m.set(f, n, p);
    }
    u32 base = 0;
    i64 index = 0;
    u64 bound = 0;
    bool single = false;
    if (Flow fl = m.pointer_index(p, imm_type(f, n), base, index, bound, single); fl != Flow::next) return fl;
    // `index` is in [0, bound], so negating it cannot overflow, while negating `by` could.
    if ((by > 0 && index > i64(bound) - by) || (by < 0 && by < -index)) return m.fail(Reason::pointer_bounds);
    i64 at = index + by;
    Value r{VKind::object, false, p.ref, 0};
    if (single) {
      r.bits = base;
      r.past = at == 1;
    } else {
      r.bits = m.child(base, {ir::Step::elem, u32(at)});
    }
    return m.set(f, n, r);
  }

  // [expr.add]/5: both pointers into the same array (or one past it); null minus null is zero.
  static Flow ptr_diff(M& m, Frame& f, N n) {
    Value a = m.operand(f, n, 0), b = m.operand(f, n, 1);
    const M::Shape* s;
    if (Flow fl = m.int_shape(f, m.result_type(f, n), s); fl != Flow::next) return fl;
    if (a.kind == VKind::null && b.kind == VKind::null) return m.set(f, n, Value::integer(0));
    if (a.kind != VKind::object || b.kind != VKind::object || a.ref != b.ref) return m.fail(Reason::pointer_difference);
    u32 ba = 0, bb = 0;
    i64 ia = 0, ib = 0;
    u64 na = 0, nb = 0;
    bool sa = false, sb = false;
    if (Flow fl = m.pointer_index(a, imm_type(f, n), ba, ia, na, sa); fl != Flow::next) return fl;
    if (Flow fl = m.pointer_index(b, imm_type(f, n), bb, ib, nb, sb); fl != Flow::next) return fl;
    if (ba != bb || sa != sb) return m.fail(Reason::pointer_difference);
    i64 d = ia - ib;
    if (!M::fits(d, s->width)) return m.fail(Reason::signed_overflow);
    return m.set(f, n, Value::integer(u64(d)));
  }

  static Flow ptr_cmp(M& m, Frame& f, N n) {
    return m.compare_pointers(f, n, ir::Pred(imms(f, n)[0]), m.operand(f, n, 0), m.operand(f, n, 1));
  }

  // A pointer to a subobject: the path is appended. Forming it needs a pointer to an object
  // (not null, not past its end); the object need not be within its lifetime.
  static Flow ptr_member(M& m, Frame& f, N n) {
    Value p = m.operand(f, n, 0);
    if (p.kind == VKind::null) return m.fail(Reason::null_access);
    if (p.kind != VKind::object) return m.fail(Reason::ill_formed);
    if (p.past) return m.fail(Reason::past_end_access);
    auto path = imm_path(f, n, 1);
    bool past = false;
    auto pl = m.resolve(p, path, past);
    if (!pl) return m.fail(Reason::pointer_bounds);
    if (!m.similar(pl->type, imm_type(f, n))) return m.fail(Reason::access_type);
    return m.set(f, n, Value{VKind::object, false, p.ref, m.extend(u32(p.bits), path)});
  }

  static Flow ptr_derived(M& m, Frame&, N n) { return m.unsupported(n); }

  // static_cast between pointers through `void*` ([expr.static.cast]/14): in constant evaluation
  // the target must be similar to the object's type ([expr.const]).
  static Flow ptr_cast(M& m, Frame& f, N n) {
    Value p = m.operand(f, n, 0);
    if (p.kind == VKind::object && constant(m)) {
      const M::Shape& rs = m.shape(m.result_type(f, n));
      QualType to = rs.inner;
      if (!rs.is_nullptr && !ast::is_void(*m.in_, to)) {
        bool past = false;
        Value whole = p;
        whole.past = false;
        auto pl = m.resolve(whole, {}, past);
        if (!pl || !m.similar(pl->type, to)) return m.fail(Reason::void_cast_type);
      }
    }
    return m.set(f, n, p);
  }

  static Flow ptr_reinterpret(M& m, Frame&, N n) { return constant(m) ? m.fail(Reason::reinterpret) : m.unsupported(n); }
  static Flow ptr_to_int(M& m, Frame&, N n) { return constant(m) ? m.fail(Reason::reinterpret) : m.unsupported(n); }
  static Flow int_to_ptr(M& m, Frame&, N n) { return constant(m) ? m.fail(Reason::reinterpret) : m.unsupported(n); }

  // ---------------------------------------------------------------- memory and lifetime

  static Flow alloc(M& m, Frame& f, N n) {
    QualType t = imm_type(f, n);
    auto storage = ir::Storage(imms(f, n)[1]);
    auto init = ir::Init(imms(f, n)[2]);
    if (storage != ir::Storage::automatic && storage != ir::Storage::temporary) return m.unsupported(n);
    if (m.shape(t).leaves == 0) return m.unsupported(n);
    u32 o = m.make_object(t, M::Origin::automatic);
    const M::Object& ob = m.objects_[o];
    if (init == ir::Init::zero) {
      m.fill_zero(t, ob.cell0);
    } else {
      M::Fill fill = init == ir::Init::erroneous ? M::Fill::erroneous : M::Fill::indeterminate;
      for (u32 c = ob.cell0; c < ob.cell0 + ob.ncells; ++c) m.cells_[c].fill = fill;
    }
    return m.set(f, n, Value{VKind::object, false, o, 0}, 1);
  }

  static Flow dealloc(M& m, Frame& f, N n) {
    Value p = m.operand(f, n, 1);
    if (p.kind != VKind::object || p.bits != 0 || p.past || !m.objects_[p.ref].storage ||
        m.objects_[p.ref].origin != M::Origin::automatic)
      return m.fail(Reason::ill_formed);
    m.objects_[p.ref].storage = false;
    return Flow::next;
  }

  // Beginning or ending the lifetime of an object from outside the evaluation is a modification
  // of it ([expr.const]).
  static Flow lifetime_change(M& m, const Value& p) {
    if (p.kind != VKind::object) return Flow::next;
    const M::Object& ob = m.objects_[p.ref];
    if (ob.read_only) return m.fail(Reason::modify_const);
    if (constant(m) && ob.outside) return m.fail(Reason::modify_outside);
    return Flow::next;
  }

  static Flow obj__begin(M& m, Frame& f, N n) {
    M::Place pl;
    if (Flow fl = m.locate(m.operand(f, n, 1), {}, imm_type(f, n), M::Use::lifetime, false, pl); fl != Flow::next) return fl;
    if (Flow fl = lifetime_change(m, m.operand(f, n, 1)); fl != Flow::next) return fl;
    for (u32 c = pl.cell0; c < pl.cell0 + pl.ncells; ++c) m.cells_[c].life = M::Life::alive;
    return Flow::next;
  }

  static Flow obj__end(M& m, Frame& f, N n) {
    Value p = m.operand(f, n, 1);
    if (p.kind != VKind::object) return m.fail(p.kind == VKind::null ? Reason::null_access : Reason::ill_formed);
    if (!m.objects_[p.ref].storage) return m.fail(Reason::storage_ended);
    bool past = false;
    auto pl = m.resolve(p, {}, past);
    if (!pl || past) return m.fail(Reason::past_end_access);
    if (Flow fl = lifetime_change(m, p); fl != Flow::next) return fl;
    for (u32 c = pl->cell0; c < pl->cell0 + pl->ncells; ++c) m.cells_[c].life = M::Life::after;
    return Flow::next;
  }

  static Flow union__activate(M& m, Frame&, N n) { return m.unsupported(n); }

  static Flow load(M& m, Frame& f, N n) {
    QualType t = imm_type(f, n);
    M::Place pl;
    if (Flow fl = m.locate(m.operand(f, n, 1), imm_path(f, n, 1), t, M::Use::read, has(f, n, ir::Flag::volatile_), pl);
        fl != Flow::next)
      return fl;
    if (pl.ncells != 1 || m.shape(t).kind == M::Kind::array) return m.unsupported(n);
    M::Cell& c = m.cells_[pl.cell0];
    if (c.fill == M::Fill::constant) {
      // A global's initial value, decoded on its first read; the node runs again if that needs
      // another global from the host.
      Value v;
      if (Flow fl = m.constant(ConstId(u32(c.v.bits)), t, v); fl != Flow::next) return fl;
      M::Cell& again = m.cells_[pl.cell0];
      again.fill = M::Fill::value;
      again.v = v;
      return m.set(f, n, v);
    }
    switch (c.fill) {
    case M::Fill::indeterminate: return m.fail(Reason::read_indeterminate);
    case M::Fill::erroneous:
      // P2795: the erroneous value, zero on every target ycxx has; not constant ([expr.const]).
      if (constant(m)) return m.fail(Reason::read_erroneous);
      return m.set(f, n, m.shape(t).kind == M::Kind::pointer    ? Value::null()
                         : m.shape(t).kind == M::Kind::floating ? Value::floating(0)
                                                                 : Value::integer(0));
    case M::Fill::value:
    case M::Fill::constant: break;
    }
    return m.set(f, n, c.v);
  }

  static Flow store(M& m, Frame& f, N n) {
    QualType t = imm_type(f, n);
    Value v = m.operand(f, n, 2);
    if (v.kind == VKind::none) return m.fail(Reason::ill_formed);
    M::Place pl;
    if (Flow fl = m.locate(m.operand(f, n, 1), imm_path(f, n, 1), t, M::Use::write, has(f, n, ir::Flag::volatile_), pl);
        fl != Flow::next)
      return fl;
    if (pl.ncells != 1 || m.shape(t).kind == M::Kind::array) return m.unsupported(n);
    M::Cell& c = m.cells_[pl.cell0];
    c.fill = M::Fill::value;
    c.v = v;
    return Flow::next;
  }

  static Flow copy(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow bit_field_load(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow bit_field_store(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow atomic__load(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow atomic__store(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow atomic__rmw(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow atomic__cmpxchg(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow fence(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow vfunc(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow rtti__typeid(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow rtti__dyncast(M& m, Frame&, N n) { return m.unsupported(n); }

  // ---------------------------------------------------------------- calls

  // A call to `e`: its body is fetched once per evaluation; in constant mode it must be a defined
  // constexpr function ([expr.const]).
  static Flow call_entity(M& m, Frame& f, N n, EntityId e, u32 first_arg) {
    if (constant(m) && has(f, n, ir::Flag::not_constexpr)) return m.fail(Reason::not_constexpr);
    auto it = m.bodies_.find(std::to_underlying(e));
    if (it == m.bodies_.end()) {
      m.wanted_ = e;
      m.pending_ = Event::need_body;
      return Flow::suspend;
    }
    if (!it->second.body) return m.fail(it->second.unsupported ? Reason::callee_unsupported : Reason::undefined_function);
    if (constant(m) && !it->second.is_constexpr) return m.fail(Reason::not_constexpr);
    return m.enter(f, n, it->second.body, e, f.fn->operands_of(n).subspan(first_arg));
  }
  static Flow call(M& m, Frame& f, N n) { return call_entity(m, f, n, EntityId(u32(imms(f, n)[0])), 1); }
  static Flow call_ind(M& m, Frame& f, N n) {
    Value c = m.operand(f, n, 1);
    if (c.kind == VKind::null) return m.fail(Reason::null_call);
    if (c.kind != VKind::function) return m.fail(Reason::ill_formed);
    return call_entity(m, f, n, EntityId(c.ref), 2);
  }
  static Flow builtin(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow asm_(M& m, Frame&, N n) { return constant(m) ? m.fail(Reason::asm_) : m.unsupported(n); }

  // ---------------------------------------------------------------- constant evaluation

  static Flow is_const_eval(M& m, Frame& f, N n) { return m.set(f, n, Value::integer(constant(m))); }

  // ---------------------------------------------------------------- unsequenced groups

  static Flow useq__begin(M& m, Frame&, N) {
    m.groups_.push_back({m.frames_.size() - 1, 0, {}});
    return Flow::next;
  }
  static Flow useq__next(M& m, Frame&, N) {
    if (m.groups_.empty() || m.groups_.back().frame != m.frames_.size() - 1) return m.fail(Reason::ill_formed);
    ++m.groups_.back().segment;
    return Flow::next;
  }
  static Flow useq__end(M& m, Frame&, N) {
    if (m.groups_.empty() || m.groups_.back().frame != m.frames_.size() - 1) return m.fail(Reason::ill_formed);
    m.groups_.pop_back();
    return Flow::next;
  }

  // ---------------------------------------------------------------- exceptions, coroutines, reflection (not yet)

  static Flow eh__match(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow eh__ptr(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow catch__begin(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow catch__end(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__begin(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__promise(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__from_promise(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__address(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__from_address(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__resume(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__destroy(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__done(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow meta__call(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow meta__effect(M& m, Frame&, N n) { return m.unsupported(n); }

  // ---------------------------------------------------------------- terminators

  static Flow br(M& m, Frame& f, N n) {
    if (constant(m) && has(f, n, ir::Flag::not_constexpr)) return m.fail(Reason::goto_);  // [expr.const]
    return m.jump(f, n, 0);
  }
  static Flow cond_br(M& m, Frame& f, N n) {
    Value c = m.operand(f, n, 0);
    if (c.kind != VKind::integer) return m.fail(Reason::ill_formed);
    return m.jump(f, n, c.bits ? 0 : 1);
  }
  static Flow switch_(M& m, Frame& f, N n) {
    Value v = m.operand(f, n, 0);
    if (v.kind != VKind::integer) return m.fail(Reason::ill_formed);
    QualType t = m.type_of(f, f.fn->operands_of(n)[0]);
    auto cases = imm_path(f, n, 0);
    for (u32 i = 0; i < cases.size(); ++i) {
      Value c;
      if (Flow fl = m.constant(ConstId(u32(cases[i])), t, c); fl != Flow::next) return fl;
      if (c.bits == v.bits) return m.jump(f, n, i + 1);
    }
    return m.jump(f, n, 0);
  }
  static Flow consteval_br(M& m, Frame& f, N n) { return m.jump(f, n, constant(m) ? 0 : 1); }

  // Returns to the caller, whose call node receives the state, then the values.
  static Flow ret(M& m, Frame& f, N n) {
    auto ops = f.fn->operands_of(n);
    std::vector<Value> vals;
    std::vector<QualType> types;
    for (ir::ValueId v : ops.subspan(1)) {
      vals.push_back(m.reg(f, v));
      types.push_back(m.type_of(f, v));
    }
    std::size_t depth = m.frames_.size() - 1;
    std::erase_if(m.groups_, [&](const M::Group& g) { return g.frame >= depth; });
    m.frames_.pop_back();  // `f` dangles from here
    if (m.frames_.empty()) {
      m.results_ = std::move(vals);
      m.result_types_ = std::move(types);
      m.pending_ = Event::done;
      return Flow::stop;
    }
    Frame& caller = m.frames_.back();
    N call{caller.node};
    auto results = caller.fn->results_of(call);
    if (results.size() != vals.size() + 1) return m.fail(Reason::ill_formed);
    for (u32 i = 0; i < vals.size(); ++i) caller.regs[ir::raw(results[i + 1])] = vals[i];
    ++caller.node;
    return Flow::returned;
  }

  static Flow unreachable(M& m, Frame& f, N n) {
    return m.fail(has(f, n, ir::Flag::not_constexpr) ? Reason::static_definition : Reason::unreachable);
  }
  static Flow invoke(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow invoke_ind(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow throw_(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow rethrow(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow resume(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__suspend(M& m, Frame&, N n) { return m.unsupported(n); }
  static Flow coro__end(M& m, Frame&, N n) { return m.unsupported(n); }
};

Event Machine::run() {
  static constexpr auto dispatch = ir::eval_table<Ops, Ops::Handler>();
  if (failed_) return Event::failed;
  if (frames_.empty()) return Event::done;
  for (;;) {
    if (config_.poll_interval && since_poll_ >= config_.poll_interval) {
      since_poll_ = 0;
      return Event::poll;
    }
    if (steps_ >= config_.step_limit) {
      fail(Reason::step_limit, config_.step_limit);
      return Event::failed;
    }
    ++steps_;
    ++since_poll_;
    Frame& f = frames_.back();
    N n{f.node};
    switch (dispatch[std::to_underlying(f.fn->node(n).op)](*this, f, n)) {
    case Flow::next: ++frames_.back().node; break;
    case Flow::jumped:
    case Flow::called:
    case Flow::returned: break;
    case Flow::suspend:
      --steps_;  // the node runs again once the host has answered
      --since_poll_;
      return pending_;
    case Flow::stop: return failed_ ? Event::failed : Event::done;
    }
  }
}

}  // namespace ycxx::interp
