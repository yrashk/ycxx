// The L1 op table (optimization design §2.1–§2.3, §10): one reflected C++26 definition. Each op is
// an enumerator of `Op`, annotated with its `OpSpec`: node class, operand and result kinds,
// immediates, successors, the node flags it accepts and its traits. Everything else (the
// printer, the parser, the verifier, the fingerprint, the evaluator hook) reads `op_table`, which
// is built from the enumerators by reflection, so adding an op is adding one line here.
//
// An op's printed name is its identifier with `__` spelled `.`: `obj__begin` is `obj.begin`.
//
// The values L1 manipulates are typed by C++ types (canonical `QualType`s) and by three IR types:
// `!state`, the threaded memory and I/O state (§2.2); `!exn`, an in-flight exception, received by
// an unwind destination; and `!coro`, a coroutine handle.
export module ycxx.ir:ops;

import std;

namespace ycxx::ir {

// The four node classes of §2.2.
export enum class NodeClass : std::uint8_t {
  pure,     // speculatable; floats
  pinned,   // pure, but may trap: stays in its block
  state,    // takes or yields `!state`; pinned by its state operand and its block
  control,  // a terminator
};

// The kind of an operand or result. `values` stands for zero or more C++ values and may only be
// last.
export enum class Arg : std::uint8_t { state, value, exn, coro, values };

// Immediate operands, in the order an op's spec lists them.
export enum class Imm : std::uint8_t {
  type,      // a canonical QualType
  entity,    // an EntityId
  constant,  // a ConstId
  u32,       // an unsigned number (vtable slot, builtin id, `final` flag)
  path,      // an access path: a list of PathSteps
  pred,      // a Pred
  storage,   // a Storage
  init,      // an Init
  order,     // a MemOrder
  rmw,       // an Rmw
  consts,    // a list of ConstIds (switch case values)
};

// Comparison predicates. `s*`/`u*` for integers, the plain ones for floating point (ordered,
// except `ne`) and pointers ([expr.rel], [expr.eq]).
export enum class Pred : std::uint8_t { eq, ne, slt, sle, sgt, sge, ult, ule, ugt, uge, lt, le, gt, ge };

// Storage duration of an `alloc` ([basic.stc]). Static and thread objects are entities, not
// allocations.
export enum class Storage : std::uint8_t { automatic, temporary, dynamic, exception };

// Initial state of an allocation's bytes ([dcl.init.general], P2795).
export enum class Init : std::uint8_t { erroneous, indeterminate, zero };

// What a `builtin` op computes; its u32 immediate. The C library builtins use this op on the
// constant edge of `consteval_br` and call the corresponding library function on the runtime edge.
//   - `memchr` (!state, s, c, n) -> (!state, pointer): the first of the n bytes at `s` equal to
//     `c` converted to unsigned char, or null; the bytes are read in order up to the match.
//   - `memcmp` (!state, a, b, n) -> (!state, int): the sign of the first difference between the
//     n bytes at `a` and at `b`, as unsigned char, or 0; both must hold n bytes.
//   - `nansl` (!state) -> (!state, long double): the reached operation diagnoses unavailable
//     signaling NaN value support.
export enum class BuiltinFn : std::uint8_t { memchr, memcmp, nansl };

// The semantic call forms represented by call_virtual. A qualified ordinary member call is
// a direct call, including during constant evaluation.
export enum class VirtualCallKind : std::uint8_t { ordinary, immediate, qualified_immediate };

export enum class MemOrder : std::uint8_t { relaxed, consume, acquire, release, acq_rel, seq_cst };
export enum class Rmw : std::uint8_t { xchg, add, sub, and_, or_, xor_, max, min };

export constexpr bool atomic_load_order(MemOrder order) {
  return order == MemOrder::relaxed || order == MemOrder::consume ||
         order == MemOrder::acquire || order == MemOrder::seq_cst;
}

export constexpr bool atomic_store_order(MemOrder order) {
  return order == MemOrder::relaxed || order == MemOrder::release || order == MemOrder::seq_cst;
}

// A compare-exchange failure is a load. It cannot be stronger than the read side of success.
export constexpr bool atomic_failure_order(MemOrder success, MemOrder failure) {
  if (success == MemOrder::release) success = MemOrder::relaxed;
  if (success == MemOrder::acq_rel) success = MemOrder::acquire;
  return atomic_load_order(failure) && failure <= success;
}

// One step of a typed access path ([basic.lval], [class.mem]): the i-th non-static data member,
// direct base, virtual base, or array element with a constant index. Union members are fields.
export enum class Step : std::uint8_t { field, base, vbase, elem };
export struct PathStep {
  Step step = Step::field;
  std::uint32_t index = 0;
  constexpr bool operator==(const PathStep&) const = default;
};

// Per-node flags (§2.3: `nsw`/`nuw`/`exact` on arithmetic; `volatile` and non-constexpr callees
// make an operation non-constant). The enumerator is the bit position.
// `call_body` marks accesses of a lowered builtin body, which are indeterminately sequenced with
// the caller's other evaluations ([intro.execution]). `explicit_trap` and `assumed_unreachable`
// mark an `unreachable` that `__builtin_trap()` and `__builtin_unreachable()` lower to.
export enum class Flag : std::uint8_t {
  nsw, nuw, exact, volatile_, not_constexpr, variadic, call_body, explicit_trap, assumed_unreachable,
  outside_lifetime,  // obj.begin imports a local whose lifetime began outside an evaluation
  outside_unknown,   // obj.begin imports a record's identity without a readable value
  unknown_reference, // the record is the unknown referent of a parameter
  count_zero_gcc,    // GCC mode returns the operand width for zero in constant evaluation
  switch_bypassed_lifetime,  // switch dispatch entered the scope of a vacuously initialized local
  observes_context,  // consteval_br of `if consteval`: its edge is the program's observation
  initializing_store,  // a compiler-generated scalar initializer for a local record array
};
export using Flags = std::uint16_t;
export constexpr Flags bit(Flag f) { return Flags(1u << std::to_underlying(f)); }

// Traits of the op itself.
export enum class Trait : std::uint8_t {
  commutative,  // operands 0 and 1 may be swapped
  nonconstant,  // never allowed in constant evaluation ([expr.const])
};
export using Traits = std::uint8_t;

export template <class T, std::size_t N>
struct List {
  std::array<T, N> at{};
  std::uint8_t size = 0;
  constexpr List() = default;
  constexpr List(std::initializer_list<T> l) {
    for (T x : l) at[size++] = x;
  }
  constexpr std::span<const T> view() const { return {at.data(), size}; }
};

export using Sig = List<Arg, 4>;
export using ImmSig = List<Imm, 4>;

export struct OpSpec {
  NodeClass cls = NodeClass::pure;
  Sig in, out;
  ImmSig imm;
  std::uint8_t succ = 0;  // successors; with `var_succ`, the minimum
  bool var_succ = false;
  Flags flags = 0;  // the node flags this op accepts
  Traits traits = 0;

  constexpr OpSpec imms(ImmSig i) const { auto s = *this; s.imm = i; return s; }
  constexpr OpSpec successors(std::uint8_t n, bool variadic = false) const {
    auto s = *this; s.succ = n; s.var_succ = variadic; return s;
  }
  constexpr OpSpec accepts(std::initializer_list<Flag> fs) const {
    auto s = *this;
    for (Flag f : fs) s.flags |= bit(f);
    return s;
  }
  constexpr OpSpec with(std::initializer_list<Trait> ts) const {
    auto s = *this;
    for (Trait t : ts) s.traits |= Traits(1u << std::to_underlying(t));
    return s;
  }
  constexpr bool has(Trait t) const { return traits & (1u << std::to_underlying(t)); }
};

namespace spec {
constexpr Arg S = Arg::state, V = Arg::value, X = Arg::exn, C = Arg::coro, Vs = Arg::values;
constexpr OpSpec make(NodeClass c, Sig in, Sig out) {
  OpSpec s;
  s.cls = c;
  s.in = in;
  s.out = out;
  return s;
}
constexpr OpSpec pure(Sig in, Sig out) { return make(NodeClass::pure, in, out); }
constexpr OpSpec pinned(Sig in, Sig out) { return make(NodeClass::pinned, in, out); }
constexpr OpSpec state(Sig in, Sig out) { return make(NodeClass::state, in, out); }
constexpr OpSpec control(Sig in, Sig out, std::uint8_t succ, bool variadic = false) {
  return make(NodeClass::control, in, out).successors(succ, variadic);
}
using enum Imm;
using enum Flag;
using enum Trait;
}  // namespace spec

using namespace spec;

export enum class Op : std::uint16_t {
  // Constants and addresses of entities.
  const_ [[=pure({}, {V}).imms({constant})]],
  global [[=pure({}, {V}).imms({entity})]],       // pointer to a variable or temporary entity
  func_addr [[=pure({}, {V}).imms({entity})]],
  vtable_addr [[=pure({}, {V}).imms({entity})]],  // the symbolic vtable of a class ([class.virtual])

  // Integer arithmetic ([expr.arith]); overflow under `nsw`/`nuw` is UB.
  add [[=pure({V, V}, {V}).accepts({nsw, nuw}).with({commutative})]],
  sub [[=pure({V, V}, {V}).accepts({nsw, nuw})]],
  mul [[=pure({V, V}, {V}).accepts({nsw, nuw}).with({commutative})]],
  neg [[=pure({V}, {V}).accepts({nsw})]],
  sdiv [[=pinned({V, V}, {V}).accepts({exact})]],
  udiv [[=pinned({V, V}, {V}).accepts({exact})]],
  srem [[=pinned({V, V}, {V})]],
  urem [[=pinned({V, V}, {V})]],
  shl [[=pure({V, V}, {V}).accepts({nsw, nuw})]],
  lshr [[=pure({V, V}, {V}).accepts({exact})]],
  ashr [[=pure({V, V}, {V}).accepts({exact})]],
  and_ [[=pure({V, V}, {V}).with({commutative})]],
  or_ [[=pure({V, V}, {V}).with({commutative})]],
  xor_ [[=pure({V, V}, {V}).with({commutative})]],
  ctz [[=pure({V}, {V}).accepts({count_zero_gcc})]],  // trailing zero bits of an integer
  icmp [[=pure({V, V}, {V}).imms({pred})]],
  select [[=pure({V, V, V}, {V})]],

  // Floating point.
  fadd [[=pure({V, V}, {V}).with({commutative})]],
  fsub [[=pure({V, V}, {V})]],
  fmul [[=pure({V, V}, {V}).with({commutative})]],
  fdiv [[=pure({V, V}, {V})]],
  fneg [[=pure({V}, {V})]],
  fabs [[=pure({V}, {V})]],  // the magnitude: the sign bit cleared, of a NaN too
  fcmp [[=pure({V, V}, {V}).imms({pred})]],

  // Conversions to the result type ([conv]).
  iconv [[=pure({V}, {V})]],   // integral, [conv.integral], [conv.prom]
  fconv [[=pure({V}, {V})]],   // floating, [conv.double], [conv.fpprom]
  f2i [[=pure({V}, {V})]],     // [conv.fpint]; out of range is UB
  i2f [[=pure({V}, {V})]],     // [conv.fpint]
  tobool [[=pure({V}, {V})]],  // [conv.bool]
  bit_cast [[=pure({V}, {V})]],

  // Pointers are (object, access path) (§2.3).
  ptr_add [[=pure({V, V}, {V}).imms({type})]],          // [expr.add], element type
  ptr_diff [[=pure({V, V}, {V}).imms({type})]],
  ptr_cmp [[=pure({V, V}, {V}).imms({pred})]],          // [expr.eq], [expr.rel]
  ptr_member [[=pure({V}, {V}).imms({type, path})]],    // subobject: member, base
  ptr_derived [[=pure({V}, {V}).imms({type, path})]],   // base to derived, undoing `path`
  ptr_cast [[=pure({V}, {V}).imms({type})]],            // static_cast through void*
  ptr_reinterpret [[=pure({V}, {V}).imms({type}).with({nonconstant})]],
  ptr_to_int [[=pure({V}, {V}).with({nonconstant})]],
  int_to_ptr [[=pure({V}, {V}).with({nonconstant})]],

  // Memory and object lifetime ([basic.life], [basic.stc], [class.union]).
  alloc [[=state({S}, {S, V}).imms({type, storage, init})]],
  dealloc [[=state({S, V}, {S}).imms({storage})]],
  obj__begin [[=state({S, V}, {S}).imms({type}).accepts({
    outside_lifetime, outside_unknown, unknown_reference, switch_bypassed_lifetime
  })]],
  obj__end [[=state({S, V}, {S})]],
  union__activate [[=state({S, V}, {S}).imms({path})]],
  load [[=state({S, V}, {V}).imms({type, path}).accepts({volatile_, call_body})]],
  store [[=state({S, V, V}, {S}).imms({type, path}).accepts({volatile_, call_body, initializing_store})]],
  bit_field_load [[=state({S, V}, {V}).imms({u32}).with({nonconstant})]],  // record pointer and field index
  bit_field_store [[=state({S, V, V}, {S}).imms({u32}).with({nonconstant})]],
  copy [[=state({S, V, V}, {S}).imms({type})]],  // trivial copy of an object: dst, src
  atomic__load [[=state({S, V}, {S, V}).imms({type, order}).accepts({volatile_}).with({nonconstant})]],
  atomic__store [[=state({S, V, V}, {S}).imms({type, order}).accepts({volatile_}).with({nonconstant})]],
  atomic__rmw [[=state({S, V, V}, {S, V}).imms({type, rmw, order}).accepts({volatile_}).with({nonconstant})]],
  // Address, expected value, desired value. The data results are the prior value and success.
  atomic__cmpxchg [[=state({S, V, V, V}, {S, V, V}).imms({type, order, order}).accepts({volatile_}).with({nonconstant})]],
  fence [[=state({S}, {S}).imms({order}).with({nonconstant})]],

  // Dynamic dispatch and RTTI. The vptr is a typed field holding `vtable_addr(C)`.
  vfunc [[=state({S, V}, {V}).imms({entity, u32})]],  // the function in a class's vtable slot
  rtti__typeid [[=state({S, V}, {V})]],
  rtti__dyncast [[=state({S, V}, {V}).imms({type})]],
  rtti__most_derived_addr [[=state({S, V}, {V})]],  // address of the most derived object from a non-null subobject pointer

  // Calls without an unwind edge: the callee cannot throw, or the exception leaves the function.
  // The first u32 is how many of the last arguments the callee's ellipsis matches
  // ([expr.call]/12); they already have the default argument promotions. A target may pass them
  // differently. The second is a mask of the arguments that are the addresses of argument objects
  // of class type, which a parameter of class type is ([expr.call]/7); the third is 1 + the
  // index of the argument that is the address of the object a class result initializes
  // ([stmt.return]), 0 if none. `lower` passes both as the target's calling convention does.
  // `variadic` records the callee type even when the ellipsis matches zero arguments.
  call [[=state({S, Vs}, {S, Vs}).imms({entity, u32, u32, u32}).accepts({not_constexpr, variadic})]],
  call_ind [[=state({S, V, Vs}, {S, Vs}).imms({u32, u32, u32}).accepts({not_constexpr, variadic})]],
  builtin [[=state({S, Vs}, {S, Vs}).imms({u32})]],  // a `BuiltinFn` handled by constant evaluation
  asm_ [[=state({S, Vs}, {S, Vs}).imms({constant}).with({nonconstant})]],

  // Constant evaluation ([expr.const]): `true` in constant mode, `false` in runtime mode.
  is_const_eval [[=pure({}, {V})]],

  // Unsequenced evaluation groups ([intro.execution]): begin, a separator between the operands,
  // end. The interpreter reports conflicting accesses in different operands as UB.
  useq__begin [[=state({S}, {S})]],
  useq__next [[=state({S}, {S})]],
  useq__end [[=state({S}, {S})]],

  // Exceptions. An unwind destination's parameters are (!state, !exn).
  eh__match [[=pure({X}, {V}).imms({type})]],  // does a handler of this type match ([except.handle])
  eh__ptr [[=pure({X}, {V})]],                  // the exception object
  catch__begin [[=state({S, X}, {S, V})]],      // enter a handler; the caught object or reference
  catch__end [[=state({S, X}, {S})]],

  // Coroutines, unsplit ([dcl.fct.def.coroutine]); promise calls are ordinary calls.
  coro__begin [[=state({S}, {S, C})]],
  coro__promise [[=pure({C}, {V})]],
  coro__from_promise [[=pure({V}, {C})]],
  coro__address [[=pure({C}, {V})]],
  coro__from_address [[=pure({V}, {C})]],
  coro__resume [[=state({S, C}, {S})]],
  coro__destroy [[=state({S, C}, {S})]],
  coro__done [[=state({S, C}, {V})]],

  // Reflection ([meta.reflection]): a call to the std::meta function named by the entity.
  meta__call [[=pinned({Vs}, {V}).imms({entity})]],
  meta__effect [[=state({S, Vs}, {S, V}).imms({entity})]],  // define_aggregate and other injections

  // Terminators.
  br [[=control({}, {}, 1).accepts({not_constexpr})]],  // not_constexpr: a `goto` ([expr.const])
  cond_br [[=control({V}, {}, 2)]],                      // true, false
  switch_ [[=control({V}, {}, 1, true).imms({consts})]],  // default, then one per case value
  ret [[=control({S, Vs}, {}, 0)]],
  // The flags distinguish an explicit trap, `__builtin_unreachable()` and a static definition
  // ([expr.const]) from falling off the end.
  unreachable [[=control({}, {}, 0).accepts({not_constexpr, explicit_trap, assumed_unreachable})]],
  consteval_br [[=control({}, {}, 2).accepts({observes_context})]],  // consteval, runtime ([stmt.if])
  // normal, unwind; the u32 as for `call`
  invoke [[=control({S, Vs}, {S, Vs}, 2).imms({entity, u32}).accepts({not_constexpr})]],
  invoke_ind [[=control({S, V, Vs}, {S, Vs}, 2).imms({u32}).accepts({not_constexpr})]],
  throw_ [[=control({S, V}, {}, 0, true).imms({type})]],  // the exception object; optional unwind
  rethrow [[=control({S}, {}, 0, true)]],
  resume [[=control({S, X}, {}, 0, true)]],               // keep unwinding
  coro__suspend [[=control({S, C}, {}, 2).imms({u32})]],  // resume, destroy; u32 is `final`
  coro__end [[=control({S}, {}, 0)]],

  // Variadic cursor operations ([cstdarg.syn]). The va_arg result carries the requested type.
  va_start [[=state({S, V}, {S}).with({nonconstant})]],
  va_arg [[=state({S, V}, {S, V}).with({nonconstant})]],
  va_copy [[=state({S, V, V}, {S}).with({nonconstant})]],
  va_end [[=state({S, V}, {S}).with({nonconstant})]],

  // Lvalue-to-rvalue conversion of a std::nullptr_t glvalue evaluates its designator
  // and yields null without loading the object ([conv.lval]).
  nullptr_value [[=state({S, V}, {V})]],

  // Reuse the returned storage for a placement new-expression before initialization ([basic.life]).
  obj__reuse [[=state({S, V}, {S}).imms({type})]],

  // Allocate and default-initialize an automatic root with an external Itanium vtable.
  alloc_peer_root [[=state({S}, {S, V}).imms({type, entity}).with({nonconstant})]],

  // The implicit object of an ordinary member call must designate valid storage ([basic.life]).
  obj__check_receiver [[=state({S, V}, {S})]],

  // Constant-evaluated signed intmax arithmetic yields the converted value, then overflow.
  sadd_overflow [[=pinned({V, V}, {V, V}).with({commutative})]],
  smul_overflow [[=pinned({V, V}, {V, V}).with({commutative})]],

  // A virtual member invocation for constant evaluation: receiver, converted arguments,
  // selected function, and VirtualCallKind ([expr.call], [expr.const.core]).
  call_virtual [[=state({S, V, Vs}, {S, Vs}).imms({entity, u32})]],

  clz [[=pure({V}, {V}).accepts({count_zero_gcc})]],  // leading zero bits of an integer
};

export constexpr std::size_t op_count = std::meta::enumerators_of(^^Op).size();

export struct OpInfo {
  std::string_view name;
  OpSpec spec;
};

// The printed form of an identifier: a trailing `_` (for keywords) is dropped and `__` is `.`.
consteval std::string_view printed(std::string_view id) {
  std::string s;
  for (std::size_t i = 0; i < id.size(); ++i) {
    if (id[i] == '_' && i + 1 < id.size() && id[i + 1] == '_') {
      s += '.';
      ++i;
    } else {
      s += id[i];
    }
  }
  if (s.ends_with('_')) s.pop_back();
  return std::define_static_string(s);
}

consteval std::array<OpInfo, op_count> build_table() {
  std::array<OpInfo, op_count> t{};
  std::size_t i = 0;
  for (auto e : std::meta::enumerators_of(^^Op)) {
    auto a = std::meta::annotations_of_with_type(e, ^^OpSpec);
    if (a.size() != 1) throw "every Op needs exactly one OpSpec annotation";
    t[i].name = printed(std::meta::identifier_of(e));
    t[i].spec = std::meta::extract<OpSpec>(a[0]);
    if (std::to_underlying(std::meta::extract<Op>(e)) != i) throw "Op enumerators must be dense";
    ++i;
  }
  return t;
}

export constexpr std::array<OpInfo, op_count> op_table = build_table();

export constexpr const OpInfo& info(Op op) { return op_table[std::to_underlying(op)]; }
export constexpr const OpSpec& spec_of(Op op) { return info(op).spec; }
export constexpr bool is_terminator(Op op) { return spec_of(op).cls == NodeClass::control; }

// Names of the small enums used as immediates and flags, printed as in `printed`.
export template <class E>
  requires std::is_enum_v<E>
constexpr std::string_view name_of(E e) {
  template for (constexpr auto x : std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (e == [:x:]) return printed(std::meta::identifier_of(x));
  }
  return {};
}

export template <class E>
  requires std::is_enum_v<E>
constexpr std::optional<E> named(std::string_view name) {
  template for (constexpr auto x : std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (name == printed(std::meta::identifier_of(x))) return [:x:];
  }
  return std::nullopt;
}

export constexpr std::optional<Op> op_named(std::string_view name) {
  for (std::size_t i = 0; i < op_count; ++i)
    if (op_table[i].name == name) return Op(i);
  return std::nullopt;
}

export template <class E>
  requires std::is_enum_v<E>
constexpr std::size_t enum_count = std::meta::enumerators_of(^^E).size();

// The L1 evaluator hook. An evaluator (the interpreter, #25) is a class whose static member
// functions are named after the ops' identifiers (`add`, `obj__begin`, `throw_`) and share one
// function type `F`. `eval_table<Impl, F>()` gathers them by reflection into an array indexed by
// `Op`; with `complete`, a missing op is a compile-time error, so a new op cannot be forgotten.
consteval std::meta::info member_named(std::meta::info cls, std::string_view id) {
  for (auto m : std::meta::members_of(cls, std::meta::access_context::unchecked()))
    if (std::meta::has_identifier(m) && std::meta::identifier_of(m) == id) return m;
  return {};
}

export template <class Impl, class F, bool complete = true>
consteval std::array<F*, op_count> eval_table() {
  std::array<F*, op_count> t{};
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^Op))) {
    constexpr auto m = member_named(^^Impl, std::meta::identifier_of(e));
    static_assert(!complete || m != std::meta::info{}, "the evaluator has no member for an op");
    if constexpr (m != std::meta::info{}) t[std::to_underlying([:e:])] = &[:m:];
  }
  return t;
}

}  // namespace ycxx::ir
