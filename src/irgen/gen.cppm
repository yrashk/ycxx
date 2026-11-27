// IR generation (optimization design §2, §11; ast.md §3.6–§3.7): `function_ir` turns the analysed
// body of a function into its L1 body, and its positions into `function_debug`.
//
// The shape follows L1's (§2.2): a CFG of blocks with arguments that threads one `!state` chain.
// - Every block but the entry takes the state as its first parameter; the entry takes the state,
//   then the function's parameters.
// - Parameter and block-scope objects use `alloc` with an initialization kind. A scalar's
//   initializing store comes before `obj.begin`, since its lifetime begins when its initialization
//   is complete ([basic.life]). Every scope exit (the end of a block, `return`, `break`,
//   `continue`) emits `obj.end` and `dealloc` in reverse declaration order.
// - Names are read and written through `load` and `store` with their type and access path.
//   Values flow between blocks only as block arguments: `&&`, `||` and `?:` join on one.
// - Signed `+ - *` and negation carry `nsw`, so overflow is UB ([expr.pre]); unsigned arithmetic
//   wraps ([basic.fundamental]). `<<` carries no flag ([expr.shift] defines it modulo 2^N).
// - The operands of an unsequenced operator are bracketed by `useq.begin/next/end` when one of
//   them writes memory and another touches it ([intro.execution]).
//
// Immediates and result types, as lower (#26) reads them:
// - `alloc [T, …]` allocates a T and yields a pointer to T; `load`/`store [T, path]` access a T at
//   `path` below the pointer's pointee; `ptr_member [T, path]` yields a pointer to the subobject,
//   of type T (array-to-pointer is `ptr_member [elem, /e0]`); `ptr_derived [T, path]` undoes
//   a base path within T; `ptr_add`/`ptr_diff [T]` step by elements of type T.
// - `global [@v]` is a pointer to the variable; `const_ [#c]` has the constant's type, and a string
//   literal's address is a `const_` of its VLvalue, of type pointer to the array.
// - `ptr_add`'s index is an integer of any type, widened by its signedness (lower does this);
//   `p - j` adds `-j` computed in `ptrdiff_t`.
// - The operands of an unsequenced operator form a `useq` group only for their own accesses: a
//   callee's accesses are indeterminately sequenced with them ([intro.execution]).
// - Shift operands keep their own promoted types; `call` yields the state, then the result unless
//   it is void.
//
// The emission order is the tree's evaluation order; `ir::Builder::finish` puts the body in
// canonical order (§2.4), so the fingerprint does not depend on it.
//
// - A class or array object is never a value: it is initialized element by element through
//   `ptr_member` (an `InitList`), or from another object by `copy` (a class glvalue converted to
//   a prvalue, the trivial copy constructor and copy assignment, [class.copy.ctor]). An object
//   whose initializer value-initializes some element is allocated `zero`, and those elements are
//   left as they are ([dcl.init.general]).
//
// α slice (#39) and δ's aggregates (#86): scalars, pointers, string literals and aggregates; no
// cleanups or exceptions, and no class parameters or results yet.
// Ordinary calls let exceptions leave the function. A certified hosted handler uses
// an `invoke` with explicit normal and exceptional continuations.
export module ycxx.irgen:gen;

import std;
import ycxx.ast;
import ycxx.ir;
import :facts;

export namespace ycxx::irgen {

namespace ir = ycxx::ir;

enum class UnwindCleanupChecks : std::uint8_t { none, all_calls, aggregate_defaults };

// What `function_ir(F, ctx)` reads (ast.md §3.6): the function's entity and `decl_sema`, its
// analysed body, and where the body sits in its item, for `Loc`s (§3.4, §8).
struct FunctionInput {
  ast::EntityId entity = ast::EntityId::none;
  ast::DeclSema decl{};
  ast::BodySema body{};
  ast::ItemId item{};
  std::uint32_t body_offset = 0;  // the declaration part's length in tokens
  // For the thunk of a constant expression (`{ return e; }`, query-engine.md §B7): the values of
  // the enclosing function's locals usable in constant expressions ([expr.const]), by local
  // index. The thunk sets up their values at entry. A supported opaque record is available
  // only as an object designator. Other locals have no usable object.
  bool thunk = false;
  std::span<const std::optional<ast::ConstId>> local_values{};
  // A non-static member function's `this` type ([expr.prim.this]): its implicit object argument,
  // the entry block's first value parameter, before the declared ones (Itanium C++ ABI §3.1).
  std::optional<ast::QualType> this_type{};
  // Its static locals initialized by code ([stmt.dcl]/3): each one's guard variable (Itanium C++
  // ABI §3.3.3), and the runtime functions the guarded initialization calls.
  struct StaticGuard {
    ast::EntityId variable = ast::EntityId::none;
    ast::EntityId guard = ast::EntityId::none;
  };
  std::span<const StaticGuard> guards{};
  ast::EntityId guard_acquire = ast::EntityId::none, guard_release = ast::EntityId::none;
  ast::EntityId atexit = ast::EntityId::none, dso = ast::EntityId::none;
  bool hosted_catch = false;  // the native frontend has certified this occurrence
  std::optional<std::uint32_t> catch_int_binding{};  // local index after the handler's return has been checked
  ast::NodeId cleanup_call = ast::NodeId::none;  // the certified hosted cleanup call
  ast::EntityId constructor_cleanup = ast::EntityId::none;  // the selected union member's destructor
  UnwindCleanupChecks unwind_cleanup_checks = UnwindCleanupChecks::none;
  bool exceptions_enabled = true;  // native lowering's language mode
  bool native_array_delete = false;  // the backend's Linux x86-64 cookie-free array deletion
  bool native_array_new = false;  // the backend's Linux x86-64 allocation in a certified frame
  std::span<const ast::EntityId> nonthrowing_functions{};  // native declarations, sorted by entity
  ast::EntityId destructor_table = ast::EntityId::none;
  // The table a dynamic class's constructor installs once its base subobject is initialized (#309).
  ast::EntityId constructor_table = ast::EntityId::none;
  // The initialized prefix of a flat aggregate while evaluating one selected default.
  struct InitializedFields {
    ast::EntityId owner = ast::EntityId::none;
    std::span<const ast::ConstId> values{};
  } initialized_fields{};
  // Record parameters, their supported reference referents, and direct automatic receivers
  // available to a thunk. Their lifetimes began outside the evaluation and their values are unknown.
  // Sorted by local index.
  std::span<const std::uint32_t> opaque_record_locals{};
  // Named record reference parameters in the thunk's enclosing frame, sorted by local index.
  std::span<const std::uint32_t> discardable_reference_parameters{};
};

struct FunctionIr {
  ir::Function body;
  ir::FunctionDebug debug;  // keyed by `body`'s node ids
};

// A constant-evaluation thunk binds one receiver while computing the byte range.
// The site is valid only in the thunk returned by constant_byte_capture_ir.
struct ConstantByteCapture {
  ast::Child receiver, size, data;
  std::uint32_t local;
  ir::NodeId site{ir::none};
};

// Namespace-scope reference initialization cannot extend the lifetime of a temporary in the
// startup function's frame (#367).
bool materializes_temporary(const ast::Ast& ast, ast::NodeId n) {
  if (n == ast::NodeId::none || ast.is<ast::ConstantProbe>(n)) return false;
  if (ast.is<ast::Materialize>(n)) return true;
  bool found = false;
  ast::dispatch<ast::Nodes>(ast::kind_of(n), [&]<class T>(std::type_identity<T>) {
    T v = ast.get<T>(n);
    ast::for_each_ref(v, [&]<class H>(const H& h) {
      if constexpr (std::is_same_v<H, ast::NodeId>) found = found || materializes_temporary(ast, h);
      else if constexpr (std::is_same_v<H, ast::ChildList>)
        for (ast::Child c : ast.children(h)) found = found || materializes_temporary(ast, c.id);
    });
  });
  return found;
}

// Fails, naming the construct, on what the α slice does not generate yet.
template <Facts F>
std::expected<FunctionIr, std::string> function_ir(const ast::Interners& in, const FunctionInput& fn, F& facts);

template <Facts F>
std::expected<FunctionIr, std::string> constant_byte_capture_ir(const ast::Interners& in, const FunctionInput& fn,
                                                               F& facts, ConstantByteCapture& capture);

}  // namespace ycxx::irgen

namespace ycxx::irgen {

using std::uint32_t;
using ast::NodeId;
using ir::ValueId;

// An lvalue designates an addressable object or a bit-field projected from its record pointer.
struct Place {
  ValueId ptr{};
  ast::QualType type{};
  std::optional<std::uint32_t> bit_field{};
};

// A node of the tree and its item-relative token offset (the red view's position, ast.md §3.5).
struct At {
  NodeId id = NodeId::none;
  uint32_t offset = 0;
};

struct Unsupported {
  std::string what;
};

template <Facts F>
class Gen {
 public:
  Gen(const ast::Interners& in, const FunctionInput& fn, F& facts,
      const std::set<uint32_t>* planned_bypasses = nullptr, ConstantByteCapture* capture = nullptr)
      : in_(in), ast_(in.ast), fn_(fn), facts_(facts), planned_bypasses_(planned_bypasses), capture_(capture) {}

  std::expected<FunctionIr, std::string> run() {
    try {
      body();
    } catch (const Unsupported& u) {
      return std::unexpected(std::format("function_ir: {} is not supported yet", u.what));
    }
    // Only functions with actual bypasses need a second emission, with their bindings available
    // from the entry block. Discover them from the same scope transitions used by final emission.
    if (!bypassed_locals_.empty()) return Gen(in_, fn_, facts_, &bypassed_locals_, capture_).run();
    auto built = b_.finish();
    if (!built) return std::unexpected("function_ir: " + built.error());
    if (capture_) capture_->site = built->node_of[ir::raw(capture_->site)];
    FunctionIr out;
    out.debug = debug_.remap(built->node_of, uint32_t(built->function.nodes.size()));
    out.body = std::move(built->function);
    out.body.byval_params = byval_params_;
    out.body.result_param = result_param_;
    return out;
  }

 private:
  // ------------------------------------------------------------------ handles and types

  ir::QualType ty(ast::QualType q) { return facts_.canonical(q); }
  ir::Ty vty(ast::QualType q) { return ir::Ty::of(ty(q)); }
  static std::uint64_t word(auto h) { return std::to_underlying(h); }
  TypeClass cls(ast::QualType q) { return facts_.classify(q); }
  bool same_type(ast::QualType a, ast::QualType b) { return facts_.canonical(a) == facts_.canonical(b); }
  bool is_signed(ast::QualType q) { return facts_.integer(q).is_signed; }
  bool is_nullptr(ast::QualType q) { return ast::builtin_of(in_, q) == ast::Builtin::nullptr_t; }

  // ------------------------------------------------------------------ emission

  ir::NodeId emit(ir::Op op, std::span<const ir::Ty> results, std::span<const ValueId> operands,
                  std::span<const std::uint64_t> imms = {}, ir::Flags flags = 0,
                  std::span<const ir::SuccRef> succs = {}) {
    ir::NodeId n = b_.emit(block_, op, flags, results, operands, imms, succs);
    debug_.loc.resize(ir::raw(n) + 1);
    debug_.loc[ir::raw(n)] = ir::Loc{fn_.item, loc_};
    return n;
  }
  // A pure node with one result.
  ValueId pure(ir::Op op, ast::QualType result, std::initializer_list<ValueId> operands,
               std::initializer_list<std::uint64_t> imms = {}, ir::Flags flags = 0) {
    ir::Ty r[] = {vty(result)};
    return b_.result(emit(op, r, std::span(operands), std::span(imms), flags));
  }
  // A node that consumes the state and yields the next one, plus `extra` results.
  ir::NodeId stateful(ir::Op op, std::span<const ir::Ty> extra, std::span<const ValueId> operands,
                      std::span<const std::uint64_t> imms = {}, ir::Flags flags = 0) {
    std::vector<ir::Ty> results{ir::Ty::state()};
    results.insert(results.end(), extra.begin(), extra.end());
    std::vector<ValueId> ops{state_};
    ops.insert(ops.end(), operands.begin(), operands.end());
    ir::NodeId n = emit(op, results, ops, imms, flags);
    state_ = b_.result(n, 0);
    return n;
  }
  ValueId constant(ast::QualType type, ast::ConstId c) { return pure(ir::Op::const_, type, {}, {word(c)}); }
  ValueId int_constant(ast::QualType type, std::int64_t v) { return constant(type, facts_.int_constant(type, v)); }
  ValueId float_constant(ast::QualType type, double v) {
    if (auto c = facts_.float_constant(type, v)) return constant(type, *c);
    throw Unsupported{"an extended floating-point format"};
  }

  // An access through a volatile glvalue is a side effect ([intro.execution]) and never constant.
  ir::Flags access_flags(ast::QualType t) {
    bool v = (std::to_underlying(ast::cv_of(in_.canonical(t))) & 2) != 0;
    return v ? ir::bit(ir::Flag::volatile_) : ir::Flags(0);
  }
  ValueId load(const Place& p, ir::Flags flags = 0) {
    ir::Ty r[] = {vty(p.type)};
    ValueId ops[] = {state_, p.ptr};
    if (p.bit_field) {
      std::uint64_t imms[] = {*p.bit_field};
      return b_.result(emit(ir::Op::bit_field_load, r, ops, imms));
    }
    std::uint64_t imms[] = {word(ty(p.type)), 0};  // the type, then an empty path
    return b_.result(emit(ir::Op::load, r, ops, imms, access_flags(p.type) | flags));
  }
  void store(const Place& p, ValueId v, ir::Flags flags = 0) {
    ValueId ops[] = {p.ptr, v};
    if (p.bit_field) {
      std::uint64_t imms[] = {*p.bit_field};
      stateful(ir::Op::bit_field_store, {}, ops, imms);
      return;
    }
    std::uint64_t imms[] = {word(ty(p.type)), 0};
    stateful(ir::Op::store, {}, ops, imms, access_flags(p.type) | flags);
  }
  // A nullptr_t initializer writes storage before the qualified object's lifetime begins.
  Place initializing_place(Place p) {
    if (is_nullptr(p.type)) p.type = ast::without_cv(ty(p.type));
    return p;
  }
  ValueId address(At a) {
    Place p = lvalue(a);
    if (p.bit_field) unsupported(a, "as an addressable object (a bit-field has no address)");
    return p.ptr;
  }

  // ------------------------------------------------------------------ blocks

  ir::BlockId new_block(std::initializer_list<ast::QualType> values = {}) {
    std::vector<ir::Ty> params{ir::Ty::state()};
    for (ast::QualType q : values) params.push_back(vty(q));
    return b_.add_block(params);
  }
  void enter(ir::BlockId b) {
    block_ = b;
    state_ = b_.param(b, 0);
    live_ = true;
  }
  ir::SuccRef edge(ir::BlockId to, std::initializer_list<ValueId> values = {}) {
    ir::SuccRef s{to, {state_}};
    s.args.insert(s.args.end(), values);
    return s;
  }
  void br(ir::BlockId to, std::initializer_list<ValueId> values = {}, ir::Flags flags = 0) {
    ir::SuccRef s[] = {edge(to, values)};
    emit(ir::Op::br, {}, {}, {}, flags, s);
    live_ = false;
  }
  void cond_br(ValueId c, ir::SuccRef t, ir::SuccRef f) {
    ValueId ops[] = {c};
    ir::SuccRef s[] = {std::move(t), std::move(f)};
    emit(ir::Op::cond_br, {}, ops, {}, 0, s);
    live_ = false;
  }
  // Code after a jump is unreachable but still generated, into a block nothing branches to, which
  // `finish` drops.
  void ensure_live() {
    if (!live_) enter(new_block());
  }

  // ------------------------------------------------------------------ objects and scopes

  Place begin_object(ast::QualType type, ir::Init init, ir::Storage storage = ir::Storage::automatic) {
    ir::Ty r[] = {vty(facts_.pointer_to(type))};
    // The allocated object retains its declared cv even when an access changes the glvalue type.
    std::uint64_t imms[] = {word(in_.canonical(type)), std::to_underlying(storage), std::to_underlying(init)};
    ir::NodeId n = stateful(ir::Op::alloc, r, {}, imms);
    return {b_.result(n, 1), type};
  }
  // An object a scope ends: its destructor (none if trivial) runs, then its lifetime and storage
  // end ([basic.life], [class.dtor]).
  struct Ending {
    Place place;
    ast::EntityId destructor = ast::EntityId::none;
    ir::Storage storage = ir::Storage::automatic;
    std::optional<uint32_t> vacuous_local = {};  // a declaration a jump can bypass
    bool conditional = false;  // a null pointer denotes a temporary that was not created
  };
  void begin_lifetime(const Place& p, ir::Flags flags = 0) {
    ValueId ops[] = {p.ptr};
    std::uint64_t imms[] = {word(ty(p.type))};
    stateful(ir::Op::obj__begin, {}, ops, imms, flags);
  }
  void start_lifetime(const Place& p, ast::EntityId destructor = ast::EntityId::none,
                      std::optional<uint32_t> vacuous_local = {}) {
    begin_lifetime(p);
    scopes_.back().push_back({p, destructor, ir::Storage::automatic, vacuous_local});
  }
  void end_lifetime(const Ending& e) {
    Place p = e.place;
    if (e.vacuous_local)
      if (auto b = jump_bindings_.find(*e.vacuous_local); b != jump_bindings_.end()) p.ptr = load(b->second);
    if (e.destructor != ast::EntityId::none) destroy(p, e.destructor);
    ValueId ops[] = {p.ptr};
    stateful(ir::Op::obj__end, {}, ops);
    std::uint64_t imms[] = {std::to_underlying(e.storage)};
    stateful(ir::Op::dealloc, {}, ops, imms);
  }
  void begin_bypassed(At at, const Ending& e, ir::Flags flags = 0) {
    // Sema has rejected bypassing nonvacuous initialization ([stmt.dcl]/2).
    if (!e.vacuous_local) unsupported(at, "past a declaration");
    if (!planned_bypasses_) {
      bypassed_locals_.insert(*e.vacuous_local);
      return;
    }
    Place p = begin_object(e.place.type, ir::Init::erroneous);
    begin_lifetime(p, flags);
    store(jump_bindings_.at(*e.vacuous_local), p.ptr);
  }
  // Calls `destructor` on the object at `p`; an array's elements, the last first ([class.dtor]).
  void destroy(const Place& p, ast::EntityId destructor) {
    if (cls(p.type) == TypeClass::array) {
      ast::QualType e = facts_.element(p.type);
      for (std::uint64_t i = facts_.bound(p.type); i-- > 0;) destroy(subobject(p, ir::Step::elem, uint32_t(i), e), destructor);
      return;
    }
    ValueId self[] = {p.ptr};
    call_fn(destructor, facts_.int_type(), self, ImplicitObject::constructor_or_destructor, true);
  }
  // Ends the objects of the scopes above `depth`, innermost first; the scopes stay open.
  void exit_scopes(std::size_t depth) {
    for (std::size_t i = scopes_.size(); i-- > depth;)
      for (auto p = scopes_[i].rbegin(); p != scopes_[i].rend(); ++p) end_lifetime(*p);
  }

  // Full-expressions ([intro.execution]): the temporaries one materializes are destroyed at its
  // end, the last first ([class.temporary]/4).
  template <class Body>
  auto full_expression(Body&& f) {
    temps_.emplace_back();
    auto r = f();
    std::vector<Ending> ts = std::move(temps_.back());
    temps_.pop_back();
    for (auto t = ts.rbegin(); t != ts.rend(); ++t) {
      if (t->conditional) if_nonnull(t->place.ptr, [&] { end_lifetime(*t); });
      else end_lifetime(*t);
    }
    return r;
  }
  void push_scope() {
    scopes_.emplace_back();
    scope_ids_.push_back(next_scope_++);
  }
  void pop_scope() {
    if (live_) exit_scopes(scopes_.size() - 1);
    drop_scope();
  }
  // Closes the innermost scope whose objects every path has already ended.
  void drop_scope() {
    scopes_.pop_back();
    scope_ids_.pop_back();
  }

  // ------------------------------------------------------------------ the function

  void body() {
    ast::QualType fty = fn_.decl.type;
    auto params = in_.seqs.elements(fn_.decl.params);
    // The entry's value parameters: `this`, then the address of the object a class result
    // initializes ([stmt.return]), then the declared ones, a parameter of class type being the
    // address of its object, which the caller initializes ([expr.call]/7).
    std::vector<ir::Ty> entry{ir::Ty::state()};
    if (fn_.this_type) entry.push_back(vty(*fn_.this_type));
    ast::QualType rt = facts_.return_type(fty);
    if (cls(rt) == TypeClass::record) {
      entry.push_back(vty(facts_.pointer_to(rt)));
      result_param_ = uint32_t(entry.size() - 1);  // 1 + its index among the value parameters
    }
    for (const ast::ParamSema& p : params) {
      if (cls(p.type) == TypeClass::record) {
        entry.push_back(vty(facts_.pointer_to(p.type)));
        byval_params_ |= 1u << (entry.size() - 2);
      } else {
        entry.push_back(vty(p.type));
      }
    }
    if (entry.size() > 33) throw Unsupported{"more than 32 parameters"};
    enter(b_.add_block(entry));
    uint32_t first = (fn_.this_type ? 2 : 1) + (result_param_ ? 1 : 0);  // the first declared parameter's block parameter
    if (fn_.this_type) this_ = b_.param(block_, 1);
    if (result_param_) result_ = Place{b_.param(block_, result_param_), rt};
    loc_ = fn_.body_offset;
    pending_constructor_table_ = this_ ? fn_.constructor_table : ast::EntityId::none;
    push_scope();
    // The parameters are the first body locals (#41), named by `LocalRef` like the others.
    slots_.assign(in_.seqs.size(fn_.body.locals), std::nullopt);
    if (slots_.size() < params.size()) throw Unsupported{"a body whose locals do not start with the parameters"};
    for (uint32_t i = 0; i < params.size(); ++i) {
      if (facts_.is_reference(params[i].type)) {
        // A reference parameter is the address of its object, not an object ([dcl.ref]).
        slots_[i] = Place{b_.param(block_, i + first), facts_.referee(params[i].type)};
        continue;
      }
      if (cls(params[i].type) == TypeClass::record) {
        // The caller's object: it begins and ends its lifetime there (Itanium C++ ABI §3.1.2).
        slots_[i] = Place{b_.param(block_, i + first), params[i].type};
        continue;
      }
      Place p = begin_object(params[i].type, ir::Init::indeterminate);
      debug_.vars.push_back({ir::LocalId(i), {last_node()}});
      store(p, b_.param(block_, i + first));
      start_lifetime(p);
      slots_[i] = p;
    }
    // The thunk imports known values and supported opaque record identities. Other
    // scalar objects have ended lifetimes and other record slots stay absent.
    for (uint32_t i = 0; fn_.thunk && i < slots_.size(); ++i) {
      if (slots_[i]) continue;
      const ast::LocalRec rec = in_.seqs.at(fn_.body.locals, i);
      if (rec.flags & ast::local_flag::requires_parameter) continue;
      ast::QualType t = rec.type;
      if (std::ranges::binary_search(fn_.opaque_record_locals, i)) {
        const bool reference = facts_.is_reference(t);
        const Place p = begin_object(reference ? facts_.referee(t) : t, ir::Init::indeterminate);
        ir::Flags flags = ir::bit(ir::Flag::outside_unknown);
        if (reference) flags |= ir::bit(ir::Flag::unknown_reference);
        begin_lifetime(p, flags);
        slots_[i] = p;
        continue;
      }
      if (cls(t) == TypeClass::record) {
        if (i >= fn_.local_values.size() || !fn_.local_values[i]) continue;
        const auto value = *fn_.local_values[i];
        if (!in_.values.is<ast::VAggregate>(value)) continue;
        const auto type = in_.values.get<ast::VAggregate>(value).type;
        if (!same_type(ast::without_cv(t), ast::without_cv(type))) continue;
        const Place p = begin_object(t, ir::Init::indeterminate);
        initialize_constant({p.ptr, type}, value);
        begin_lifetime(p, ir::bit(ir::Flag::outside_lifetime));
        slots_[i] = p;
        continue;
      }
      if (cls(t) == TypeClass::array || facts_.is_reference(t) ||
          in_.types.is<ast::TMemberPtr>(in_.types.canon(t)))
        continue;
      // An enclosing automatic nullptr_t can be converted without reading its object (CWG 2907).
      // Give that synthetic object ordinary automatic storage's initial fill ([basic.indet]).
      bool automatic_nullptr = rec.entity == ast::EntityId::none && is_nullptr(t);
      Place p = begin_object(t, automatic_nullptr ? ir::Init::erroneous : ir::Init::indeterminate);
      if (i < fn_.local_values.size() && fn_.local_values[i]) {
        store(initializing_place(p), constant(t, *fn_.local_values[i]));
        start_lifetime(p);
      } else {
        ValueId ops[] = {p.ptr};
        std::uint64_t imms[] = {word(ty(p.type))};
        stateful(ir::Op::obj__begin, {}, ops, imms);
        stateful(ir::Op::obj__end, {}, ops);
      }
      slots_[i] = p;
    }
    // The binding dominates every jump target. The object itself gets fresh storage each time
    // its lifetime begins ([stmt.dcl]/2).
    if (planned_bypasses_)
      for (uint32_t local : *planned_bypasses_) {
        ast::QualType type = in_.seqs.at(fn_.body.locals, local).type;
        Place binding = begin_object(facts_.pointer_to(type), ir::Init::indeterminate);
        start_lifetime(binding);
        jump_bindings_.emplace(local, binding);
      }
    if (capture_) {
      full_expression([&] {
        const At thunk{fn_.body.tree, fn_.body_offset};
        slots_[capture_->local] = lvalue(child(thunk, capture_->receiver));
        const ValueId range[] = {rvalue(child(thunk, capture_->size)), rvalue(child(thunk, capture_->data))};
        // This generated call is observed by the interpreter before the receiver is destroyed.
        const std::uint64_t imms[] = {word(ast::EntityId::none), 0, 0, 0};
        capture_->site = stateful(ir::Op::call, {}, range, imms);
        return 0;
      });
    } else stmt({fn_.body.tree, fn_.body_offset});
    if (!gotos_.empty()) throw Unsupported{"a goto to a label that is not defined"};  // sema rejects it
    if (live_) {
      // Flowing off the end ([stmt.return]): `main` returns 0 ([basic.start.main]); any other
      // function that returns a value has undefined behaviour.
      ast::QualType ret = facts_.return_type(fty);
      if (cls(ret) == TypeClass::void_) {
        ret_({});
      } else if (facts_.is_main(fn_.entity)) {
        ValueId zero = int_constant(ret, 0);
        ret_(zero);
      } else {
        emit(ir::Op::unreachable, {}, {});
        live_ = false;
      }
    }
  }

  ir::NodeId last_node() const { return ir::NodeId(uint32_t(debug_.loc.size() - 1)); }

  void ret_(std::optional<ValueId> v) {
    exit_scopes(0);
    // A destructor's members are destroyed after its body, on every way out ([class.dtor]/13).
    for (At m : epilogue_) {
      auto d = get<ast::DestroyMember>(m);
      Place self{*this_, facts_.pointee(*fn_.this_type)};
      destroy(d.field == ~0u ? subobject(self, ir::Step::base, std::to_underlying(facts_.class_entity(d.type)), d.type)
                             : subobject(self, ir::Step::field, d.field, d.type),
              d.destructor);
    }
    std::vector<ValueId> ops{state_};
    if (v) ops.push_back(*v);
    emit(ir::Op::ret, {}, ops);
    live_ = false;
  }

  // ------------------------------------------------------------------ statements

  template <class T>
  T get(At a) {
    return ast_.get<T>(a.id);
  }
  template <class T>
  bool is(At a) {
    return ast_.is<T>(a.id);
  }
  static At child(At parent, ast::Child c) { return {c.id, parent.offset + c.at}; }
  std::string kind_name(At a) {
    std::string name;
    ast::dispatch<ast::Nodes>(ast::kind_of(a.id), [&]<class T>(std::type_identity<T>) {
      name = std::meta::identifier_of(^^T);
    });
    return name;
  }
  [[noreturn]] void unsupported(At a, std::string_view what = {}) {
    throw Unsupported{what.empty() ? kind_name(a) : std::format("{} {}", kind_name(a), what)};
  }

  // Positions every node emitted while `a` is generated at `a`, and restores the outer position.
  // Inside another item's tree (`pinned_`), every node keeps the outer position.
  struct Position {
    Gen& g;
    uint32_t saved;
    Position(Gen& gen, uint32_t at) : g(gen), saved(gen.loc_) {
      if (g.pinned_ == 0) g.loc_ = at;
    }
    ~Position() { g.loc_ = saved; }
  };
  template <class Body>
  auto pinned(Body&& f) {
    ++pinned_;
    struct Unpin {
      int& n;
      ~Unpin() { --n; }
    } unpin{pinned_};
    return f();
  }

  template <class Body>
  auto with_default_source(At a, Body&& f) {
    const auto context = get<ast::DefaultSourceContext>(a);
    struct Restore {
      uint32_t& line;
      uint32_t saved_line;
      ast::ConstId& descriptor;
      ast::ConstId saved_descriptor;
      ~Restore() {
        line = saved_line;
        descriptor = saved_descriptor;
      }
    } restore{source_line_, source_line_, source_descriptor_, source_descriptor_};
    if (source_line_ == 0) source_line_ = context.line;
    if (source_descriptor_ == ast::ConstId::none) source_descriptor_ = context.descriptor_address;
    return f(child(a, context.arg));
  }

  // A substatement is in a scope of its own even if it is not a compound statement ([stmt.pre]).
  void substmt(At a) {
    push_scope();
    stmt(a);
    pop_scope();
  }

  void hosted_catch(At a) {
    const auto trial = get<ast::TryBlock>(a);
    const At protected_body = child(a, trial.body);
    const At normal_return = child(protected_body, ast_.children(get<ast::Compound>(protected_body).stmts).front());
    const At call_at = child(normal_return, get<ast::Return>(normal_return).value);
    const auto call = get<ast::Call>(call_at);
    const At handler_at = child(a, ast_.children(trial.handlers).front());
    const auto handler = get<ast::Handler>(handler_at);
    const auto normal = b_.add_block({});
    const ir::Ty caught[] = {ir::Ty::state(), ir::Ty::exn()};
    const auto exceptional = b_.add_block(caught);
    const ValueId arg = argument(child(call_at, ast_.children(call.args).front()));
    const ValueId args[] = {state_, arg};
    const ir::Ty results[] = {ir::Ty::state(), vty(call.info.type)};
    const std::uint64_t imms[] = {word(call.function), 0};
    const ir::SuccRef succs[] = {{normal, {}}, {exceptional, {}}};
    ir::NodeId invoke;
    {
      Position position(*this, call_at.offset);
      invoke = emit(ir::Op::invoke, results, args, imms, ir::bit(ir::Flag::not_constexpr), succs);
    }

    // The normal edge has no arguments and carries the invoke's own state and value.
    block_ = normal;
    state_ = b_.result(invoke, 0);
    ret_(b_.result(invoke, 1));

    enter(exceptional);
    const ValueId exception[] = {b_.param(exceptional, 1)};
    const auto binding = fn_.catch_int_binding;
    const auto type = binding ? ast::without_cv(in_.canonical(in_.seqs.at(fn_.body.locals, *binding).type)) : handler.type;
    const ir::Ty object[] = {vty(facts_.pointer_to(type))};
    const auto begin = stateful(ir::Op::catch__begin, object, exception);
    if (binding) {
      // [except.handle]: the by-value binding only supplies this return value.
      // Take its copy while the adjusted exception object is still alive.
      const auto value = load({b_.result(begin, 1), type});
      stateful(ir::Op::catch__end, {}, exception);
      ret_(value);
      return;
    }
    stateful(ir::Op::catch__end, {}, exception);
    stmt(child(handler_at, handler.body));
  }

  // Itanium ABI §2.5.2: offset-to-top and RTTI precede the table's address point, which the
  // vptr at offset zero of `*this` holds.
  void install_vptr(ast::EntityId table_entity) {
    const auto byte = ast::builtin(facts_.interners(), ast::Builtin::unsigned_char);
    const auto table_type = facts_.pointer_to(byte);
    const auto vptr_type = facts_.pointer_to(table_type);
    const auto table = pure(ir::Op::global, table_type, {}, {word(table_entity)});
    const auto header_size = int_constant(facts_.ptrdiff_type(), 2 * ast::x86_64_linux.pointer_size);
    const auto address_point = pure(ir::Op::ptr_add, table_type, {table, header_size}, {word(ty(byte))});
    const auto vptr = pure(ir::Op::ptr_cast, vptr_type, {*this_}, {word(ty(vptr_type))});
    store({vptr, table_type}, address_point);
  }

  void stmt(At a) {
    if (a.id == NodeId::none) return;
    ensure_live();
    Position here(*this, a.offset);
    // [class.cdtor], [class.base.init]: a dynamic class's constructor installs its own table
    // after its base subobject and before its members. A delegating constructor's target does.
    if (pending_constructor_table_ != ast::EntityId::none && a.id != fn_.body.tree &&
        !(is<ast::MemberInit>(a) && get<ast::MemberInit>(a).field == ~0u)) {
      if (!is<ast::DelegatingInit>(a)) install_vptr(pending_constructor_table_);
      pending_constructor_table_ = ast::EntityId::none;
    }
    if (is<ast::Compound>(a)) {
      auto s = get<ast::Compound>(a);
      push_scope();
      for (ast::Child c : ast_.children(s.stmts)) stmt(child(a, c));
      pop_scope();
    } else if (is<ast::TryBlock>(a)) {
      if (fn_.hosted_catch) hosted_catch(a);
      else unsupported(a, "with a retained try block");
    } else if (is<ast::ExprStmt>(a)) {
      full_expression([&] {
        discard(child(a, get<ast::ExprStmt>(a).expr));
        return 0;
      });
    } else if (is<ast::MemberInit>(a)) {
      // [class.base.init]: the member subobject of `*this`.
      auto m = get<ast::MemberInit>(a);
      if (!this_) unsupported(a, "outside a constructor");
      // A field of `~0u` is the base class subobject of type `type`.
      Place self{*this_, facts_.pointee(*fn_.this_type)};
      Place p = m.field == ~0u ? subobject(self, ir::Step::base, std::to_underlying(facts_.class_entity(m.type)), m.type)
                               : subobject(self, ir::Step::field, m.field, m.type);
      full_expression([&] {
        initialize(p, child(a, m.init), false);
        return 0;
      });
      if (fn_.constructor_cleanup != ast::EntityId::none) initialized_member_ = p;
    } else if (is<ast::DelegatingInit>(a)) {
      if (!this_) unsupported(a, "outside a constructor");
      auto d = get<ast::DelegatingInit>(a);
      full_expression([&] {
        // [class.base.init]: the target constructs the same object. Constant evaluation of
        // delegating class objects is tracked in #25.
        call_constructor(*this_, child(a, d.call), false, ir::bit(ir::Flag::not_constexpr));
        return 0;
      });
    } else if (is<ast::Destructor>(a)) {
      auto d = get<ast::Destructor>(a);
      // [class.cdtor]: virtual calls during destruction use the destructor's class.
      if (fn_.destructor_table != ast::EntityId::none) install_vptr(fn_.destructor_table);
      for (ast::Child c : ast_.children(d.members)) epilogue_.push_back(child(a, c));
      stmt(child(a, d.body));
    } else if (is<ast::DeclStmt>(a)) {
      for (ast::Child c : ast_.children(get<ast::DeclStmt>(a).decls)) {
        At v = child(a, c);
        Position at(*this, v.offset);
        if (is<ast::StaticVar>(v)) {
          static_var(v);
          continue;
        }
        if (!is<ast::LocalVar>(v)) unsupported(v, "in a declaration statement");
        local_var(v);
      }
    } else if (is<ast::Return>(a)) {
      auto r = get<ast::Return>(a);
      std::optional<ValueId> v;
      // A void operand is evaluated and the function returns nothing ([stmt.return]); a
      // reference result is the address of its object. The operand's temporaries end first.
      At value = child(a, r.value);
      const bool was_selecting = selecting_initializer_;
      const bool was_selecting_return = selecting_return_;
      if (result_ && r.value.id != NodeId::none) {
        const At written = strip_parens(value);
        const bool selection = !F::evaluating_constants && !fn_.thunk &&
                               is<ast::Conditional>(written) && class_prvalue(info(written));
        const bool sequence = fn_.thunk && is<ast::OrderedBinary>(written) &&
                              get<ast::OrderedBinary>(written).op == ast::OrderedOp::comma;
        if (selection) {
          selecting_initializer_ = true;
          selecting_return_ = true;
        }
        // A class result initializes the caller's object ([stmt.return], [dcl.init.general]/16.6).
        full_expression([&] {
          // The immediate evaluator can also sequence a static member's receiver.
          initialize(*result_, value, false, selection || sequence);
          // The selected arm's temporaries join the condition's in this full-expression.
          if (selection) begin_lifetime(*result_);
          return 0;
        });
      } else if (r.value.id != NodeId::none) {
        v = full_expression([&] { return is_lvalue(value) ? std::optional(address(value)) : rvalue_or_void(value); });
      }
      ret_(v);
      selecting_initializer_ = was_selecting;
      selecting_return_ = was_selecting_return;
    } else if (is<ast::If>(a)) {
      if_(a);
    } else if (is<ast::IfConsteval>(a)) {
      if_consteval(a);
    } else if (is<ast::Asm>(a)) {
      const std::uint64_t text[] = {word(get<ast::Asm>(a).text)};
      stateful(ir::Op::asm_, {}, {}, text);
    } else if (is<ast::StaticAssert>(a)) {
      // Checked when it was analysed; it has no effect ([dcl.pre]).
    } else if (is<ast::While>(a)) {
      auto w = get<ast::While>(a);
      loop(a, ast::Child{}, w.condition, ast::Child{}, w.body, false);
    } else if (is<ast::Do>(a)) {
      auto d = get<ast::Do>(a);
      loop(a, ast::Child{}, d.condition, ast::Child{}, d.body, true);
    } else if (is<ast::For>(a)) {
      auto f = get<ast::For>(a);
      loop(a, f.init, f.condition, f.increment, f.body, false);
    } else if (is<ast::StaticInit>(a)) {
      static_init(a);
    } else if (is<ast::Break>(a) || is<ast::Continue>(a)) {
      bool brk = is<ast::Break>(a);
      if (loops_.empty() || (!brk && !loops_.back().in_loop)) unsupported(a, "outside a loop");
      const Loop& l = loops_.back();
      exit_scopes(brk ? l.depth : l.continue_depth);
      br(brk ? l.exit : l.next);
    } else if (is<ast::Switch>(a)) {
      switch_(a);
    } else if (is<ast::Case>(a) || is<ast::Default>(a)) {
      case_label(a);
    } else if (is<ast::Label>(a)) {
      label(a);
    } else if (is<ast::Goto>(a)) {
      goto_(a);
    } else {
      unsupported(a);
    }
  }

  void local_var(At a) {
    auto v = get<ast::LocalVar>(a);
    ast::LocalRec rec = in_.seqs.at(fn_.body.locals, v.local);
    if (rec.entity != ast::EntityId::none) unsupported(a, "with static or thread storage duration");
    bool has_init = v.init.id != NodeId::none;
    if (facts_.is_reference(rec.type)) {
      // A reference is bound to the object its initializer designates ([dcl.init.ref]); a
      // temporary bound to it lives as long as it ([class.temporary]/6).
      if (!has_init) unsupported(a, "of reference type without an initializer");
      slots_[v.local] = full_expression([&] { return Place{address(child(a, v.init)), facts_.referee(rec.type)}; });
      return;
    }
    const At initializer = child(a, v.init);
    const bool peer_root = has_init && is<ast::PeerRootInit>(initializer);
    const bool selection = has_init && !peer_root && selects_class_initializer(initializer);
    bool zeroed = has_init && value_initializes(initializer);
    // Conversion of nullptr_t during initialization does not access its object ([conv.lval]).
    // Its storage starts with erroneous bytes ([basic.indet]).
    bool indeterminate = has_init && !zeroed && !is_nullptr(rec.type) && !default_initializes(initializer);
    Place p;
    if (peer_root) {
      Position here(*this, initializer.offset);
      if (fn_.thunk) unsupported(initializer, "in constant evaluation");
      const auto init = get<ast::PeerRootInit>(initializer);
      ir::Ty results[] = {vty(facts_.pointer_to(rec.type))};
      std::uint64_t imms[] = {word(ty(rec.type)), std::to_underlying(init.vtable)};
      const auto alloc = stateful(ir::Op::alloc_peer_root, results, {}, imms);
      p = {b_.result(alloc, 1), rec.type};
    } else {
      p = begin_object(rec.type, zeroed ? ir::Init::zero
                                  : indeterminate ? ir::Init::indeterminate
                                                  : ir::Init::erroneous);
    }
    debug_.vars.push_back({ir::LocalId(v.local), {last_node()}});
    // The name is in scope in its own initializer ([basic.scope.pdecl]); reading it there reads an
    // object whose lifetime has not begun.
    slots_[v.local] = p;
    if (auto b = jump_bindings_.find(v.local); b != jump_bindings_.end()) store(b->second, p.ptr);
    if (has_init && !peer_root) {
      selecting_initializer_ = selection;
      full_expression([&] {
        initialize(initializing_place(p), initializer, zeroed, selection);
        if (selection) begin_lifetime(p);
        return 0;
      });
      selecting_initializer_ = false;
    }
    bool vacuous = v.destructor == ast::EntityId::none && (!has_init || is<ast::DefaultInit>(child(a, v.init)));
    if (selection) scopes_.back().push_back({p, v.destructor});
    else start_lifetime(p, v.destructor, vacuous ? std::optional(v.local) : std::nullopt);
  }

  bool class_selection(At e) {
    e = strip_parens(e);
    return (is<ast::Conditional>(e) ||
            (is<ast::OrderedBinary>(e) && get<ast::OrderedBinary>(e).op == ast::OrderedOp::comma)) &&
           class_prvalue(info(e));
  }

  // Follow the value of a class selection so a discarded bit cast still has
  // a result object. A supported cast proves trivial destruction for the result type.
  bool has_bit_cast_result(At e) {
    e = strip_parens(e);
    if (is<ast::BitCast>(e)) {
      const auto cast = get<ast::BitCast>(e);
      return cast.supported && class_prvalue(cast.info);
    }
    if (!class_selection(e)) return false;
    if (is<ast::OrderedBinary>(e)) return has_bit_cast_result(child(e, get<ast::OrderedBinary>(e).rhs));
    const auto choice = get<ast::Conditional>(e);
    return has_bit_cast_result(child(e, choice.then_)) || has_bit_cast_result(child(e, choice.else_));
  }

  // A later aggregate initializer clause can run before earlier temporaries are destroyed.
  // Check the whole initializer for throwing evaluation and cleanup when it contains a selection.
  bool selects_class_initializer(At e) {
    e = strip_parens(e);
    if (class_selection(e)) return true;
    if (is<ast::DefaultArg>(e)) return selects_class_initializer(child(e, get<ast::DefaultArg>(e).arg));
    if (is<ast::DefaultSourceContext>(e)) return selects_class_initializer(child(e, get<ast::DefaultSourceContext>(e).arg));
    if (is<ast::DefaultMemberInit>(e))
      return selects_class_initializer(child(e, get<ast::DefaultMemberInit>(e).init));
    if (!is<ast::InitList>(e)) return false;
    for (ast::Child c : ast_.children(get<ast::InitList>(e).elems))
      if (selects_class_initializer(child(e, c))) return true;
    return false;
  }

  struct ArmExit {
    ir::BlockId block;
    ValueId state;
    std::optional<ValueId> value;
    std::vector<Ending> temporaries{};
    bool reaches_join = true;
  };

  ArmExit evaluate_arm(ir::BlockId entry, auto&& eval) {
    enter(entry);
    temps_.emplace_back();
    std::optional<ValueId> value = eval();
    ArmExit arm{block_, state_, value, std::move(temps_.back()), live_};
    temps_.pop_back();
    if (!arm.reaches_join) arm.temporaries.clear();
    if (!arm.temporaries.empty() && temps_.empty()) throw Unsupported{"a temporary outside a full-expression"};
    return arm;
  }

  struct ShortCircuit {
    ir::BlockId rhs;
    bool is_and;
  };

  // Each edge supplies its result and the addresses of the temporaries it created. Other
  // addresses are null. The enclosing full-expression ends the live objects in reverse order.
  std::optional<ValueId> join_arms(std::span<ArmExit> arms, std::optional<ast::QualType> result_type,
                                   std::optional<ShortCircuit> shortcut = {}) {
    std::vector<ir::Ty> parameters{ir::Ty::state()};
    if (result_type) parameters.push_back(vty(*result_type));
    for (const auto& arm : arms)
      for (const auto& temporary : arm.temporaries)
        parameters.push_back(vty(facts_.pointer_to(temporary.place.type)));
    const auto join = b_.add_block(parameters);
    for (const auto& arm : arms) {
      if (!arm.reaches_join) continue;
      block_ = arm.block;
      state_ = arm.state;
      ir::SuccRef next{join, {state_}};
      if (arm.value) next.args.push_back(*arm.value);
      for (const auto& source : arms)
        for (const auto& temporary : source.temporaries)
          next.args.push_back(&arm == &source ? temporary.place.ptr : zero(facts_.pointer_to(temporary.place.type)));
      if (shortcut && &arm == &arms.front()) {
        // The left operand can jump straight to the join with its own boolean result.
        if (shortcut->is_and) cond_br(*arm.value, edge(shortcut->rhs), std::move(next));
        else cond_br(*arm.value, std::move(next), edge(shortcut->rhs));
      } else {
        emit(ir::Op::br, {}, {}, {}, 0, std::span(&next, 1));
      }
    }
    enter(join);
    uint32_t index = result_type ? 2 : 1;
    for (auto& arm : arms)
      for (auto& temporary : arm.temporaries) {
        temporary.place.ptr = b_.param(join, index++);
        temporary.conditional = true;
        temps_.back().push_back(temporary);
      }
    return result_type ? std::optional(b_.param(join, 1)) : std::nullopt;
  }

  // A class prvalue initializes its result object directly ([dcl.init], [class.copy.elision]).
  void initialize_selection(const Place& p, At e, bool zeroed) {
    if (is<ast::OrderedBinary>(e)) {
      const auto comma = get<ast::OrderedBinary>(e);
      discard(child(e, comma.lhs));
      return initialize(p, child(e, comma.rhs), zeroed, true);
    }
    const auto choice = get<ast::Conditional>(e);
    if (selecting_return_ && (choice.info.flags & ast::Conditional::unchecked_class_copy))
      unsupported(e, "with an unresolved class copy in a return selection");
    const auto then_value = child(e, choice.then_), else_value = child(e, choice.else_);
    if (!class_prvalue(info(then_value)) || !class_prvalue(info(else_value)))
      unsupported(e, "with a non-value arm in a class initializer");
    const auto condition = rvalue(child(e, choice.condition));
    const auto then_b = new_block(), else_b = new_block();
    cond_br(condition, edge(then_b), edge(else_b));
    auto init = [&](ir::BlockId block, At value) {
      return evaluate_arm(block, [&] {
        initialize(p, value, zeroed, true);
        return std::nullopt;
      });
    };
    ArmExit arms[] = {init(then_b, then_value), init(else_b, else_value)};
    join_arms(arms, std::nullopt);
  }

  // Whether initializer `e` value-initializes some element (`ValueInit`), so the object is
  // allocated zero.
  bool value_initializes(At e) {
    if (is<ast::ValueInit>(e)) return true;
    if (is<ast::CtorCall>(e)) return get<ast::CtorCall>(e).zero_first != 0;
    if (is<ast::Paren>(e)) return value_initializes(child(e, get<ast::Paren>(e).operand));
    if (is<ast::DefaultArg>(e)) return value_initializes(child(e, get<ast::DefaultArg>(e).arg));
    if (is<ast::DefaultSourceContext>(e)) return value_initializes(child(e, get<ast::DefaultSourceContext>(e).arg));
    if (is<ast::OrderedBinary>(e)) {
      const auto binary = get<ast::OrderedBinary>(e);
      if (binary.op == ast::OrderedOp::comma) return value_initializes(child(e, binary.rhs));
    }
    if (!is<ast::InitList>(e)) return false;
    for (ast::Child c : ast_.children(get<ast::InitList>(e).elems))
      if (value_initializes(child(e, c))) return true;
    return false;
  }

  // Whether initializer `e` leaves some element default-initialized (`DefaultInit`): its value
  // is erroneous ([basic.indet]).
  bool default_initializes(At e) {
    if (is<ast::DefaultInit>(e)) return true;
    if (!is<ast::InitList>(e)) return false;
    for (ast::Child c : ast_.children(get<ast::InitList>(e).elems))
      if (default_initializes(child(e, c))) return true;
    return false;
  }

  // The subobject `index` of the object `p` points to: an array element or a non-static data
  // member of type `t` ([intro.object]).
  Place subobject(const Place& p, ir::Step step, uint32_t index, ast::QualType t) {
    if (step == ir::Step::field && facts_.bit_field_width(facts_.class_entity(p.type), index) != 0)
      return {p.ptr, t, index};
    std::uint64_t path[] = {1, ir::encode({step, index})};
    return {pure(ir::Op::ptr_member, facts_.pointer_to(t), {p.ptr}, {word(ty(t)), path[0], path[1]}), t};
  }

  // The enclosing object recovered by undoing a checked nonvirtual base path ([expr.static.cast]).
  ValueId derived_address(At a, ast::QualType derived, bool pointer) {
    ast::QualType result = facts_.pointer_to(derived);
    std::vector<std::uint64_t> imms{word(ty(derived)), 0};
    do {
      const auto c = get<ast::ImplicitConversion>(a);
      imms.push_back(ir::encode({ir::Step::base, std::to_underlying(c.function)}));
      a = child(a, c.operand);
    } while (is<ast::ImplicitConversion>(a) &&
             get<ast::ImplicitConversion>(a).conversion == ast::ConversionKind::base_to_derived);
    imms[1] = imms.size() - 2;
    auto adjust = [&](ValueId base) {
      ir::Ty results[] = {vty(result)};
      ValueId operands[] = {base};
      return b_.result(emit(ir::Op::ptr_derived, results, operands, imms));
    };
    if (!pointer) return adjust(address(a));

    // Test the evaluated pointer once. Only the non-null path has a base subobject to adjust.
    ast::QualType from = info(a).type;
    ValueId base = rvalue(a);
    ValueId null = pure(ir::Op::ptr_cmp, facts_.bool_type(), {base, constant(from, facts_.null_constant(from))},
                        {std::to_underlying(ir::Pred::eq)});
    ValueId null_result = constant(result, facts_.null_constant(result));
    ir::BlockId nonnull = new_block({from}), join = new_block({result});
    cond_br(null, edge(join, {null_result}), edge(nonnull, {base}));
    enter(nonnull);
    br(join, {adjust(b_.param(nonnull, 1))});
    enter(join);
    return b_.param(join, 1);
  }

  // [dcl.struct.bind]/1: copy in element order and retain each access's own cv-qualifiers.
  void copy_array(const Place& dst, const Place& src) {
    Place dst_first = subobject(dst, ir::Step::elem, 0, facts_.element(dst.type));
    Place src_first = subobject(src, ir::Step::elem, 0, facts_.element(src.type));
    ast::QualType index_type = facts_.ptrdiff_type();
    ValueId count = int_constant(index_type, std::int64_t(facts_.bound(dst.type)));
    ir::BlockId head = new_block({index_type}), body = new_block(), done = new_block();
    br(head, {int_constant(index_type, 0)});
    enter(head);
    ValueId index = b_.param(head, 1);
    cond_br(pure(ir::Op::icmp, facts_.bool_type(), {index, count}, {std::to_underlying(ir::Pred::slt)}), edge(body), edge(done));
    enter(body);
    auto element = [&](const Place& first) -> Place {
      return {pure(ir::Op::ptr_add, facts_.pointer_to(first.type), {first.ptr, index}, {word(ty(first.type))}), first.type};
    };
    Place dst_element = element(dst_first), src_element = element(src_first);
    if (cls(dst_element.type) == TypeClass::array) copy_array(dst_element, src_element);
    else store(dst_element, load(src_element));
    br(head, {arith(ast::BinaryOp::add, index_type, index, int_constant(index_type, 1))});
    enter(done);
  }

  // [class.ctor]: the constructor initializes the supplied object in place.
  void call_constructor(ValueId object, At e, bool zeroed, ir::Flags call_flags = 0) {
    Position here(*this, e.offset);
    if (!is<ast::CtorCall>(e)) unsupported(e, "as a constructor target");
    auto c = get<ast::CtorCall>(e);
    if (c.zero_first && !zeroed) zero_fill({object, info(e).type});
    std::vector<ValueId> args{object};
    uint32_t byval = 0;
    for (ast::Child arg : ast_.children(c.args)) {
      At x = child(e, arg);
      if (argument_object(x)) byval |= 1u << args.size();
      args.push_back(argument(x));
    }
    call_fn(c.ctor, facts_.int_type(), args, ImplicitObject::constructor_or_destructor, true, byval, 0, call_flags);
  }

  // A folded immediate invocation has a structural value. Initialize its result place
  // by field and element, so a later materialization obtains its own typed storage.
  void initialize_constant(const Place& p, ast::ConstId value) {
    const TypeClass kind = cls(p.type);
    if (kind != TypeClass::array && kind != TypeClass::record) {
      store(p, constant(p.type, value));
      return;
    }
    if (!in_.values.is<ast::VAggregate>(value)) throw Unsupported{"an aggregate constant of the wrong kind"};
    const auto aggregate = in_.values.get<ast::VAggregate>(value);
    const auto elements = in_.seqs.elements(aggregate.elems);
    if (kind == TypeClass::array) {
      if (elements.size() != facts_.bound(p.type)) throw Unsupported{"an array constant with the wrong bound"};
      const ast::QualType type = facts_.element(p.type);
      for (uint32_t i = 0; i < elements.size(); ++i)
        initialize_constant(subobject(p, ir::Step::elem, i, type), elements[i]);
      return;
    }
    const ast::EntityId owner = facts_.class_entity(p.type);
    if (!same_type(p.type, aggregate.type) || !facts_.bases_of(owner).empty() ||
        facts_.field_type(owner, uint32_t(elements.size())))
      throw Unsupported{"a record constant with a different layout"};
    for (uint32_t i = 0; i < elements.size(); ++i) {
      const auto type = facts_.field_type(owner, i);
      if (!type) throw Unsupported{"a record constant with a different layout"};
      initialize_constant(subobject(p, ir::Step::field, i, *type), elements[i]);
    }
  }

  // [expr.spaceship]: compares the operands once, then copies the comparison category's static
  // member that names the result ([cmp.categories]): less, equal, greater, or for a NaN operand
  // (a partial ordering's fourth member) unordered.
  void three_way(const Place& p, At e) {
    const auto tw = get<ast::ThreeWay>(e);
    const At l = child(e, tw.lhs), r = child(e, tw.rhs);
    ValueId x{}, y{};
    unsequenced(std::array{l, r}, [&](std::size_t i, At o) { (i ? y : x) = rvalue(o); });
    const auto results = in_.seqs.elements(tw.results);
    if (results.size() < 3) throw Unsupported{"a comparison category without its values"};
    const ast::QualType ot = info(l).type, b = facts_.bool_type(), ptr = facts_.pointer_to(p.type);
    // `x < y`, `x == y` or `x > y` in the operands' common type, as `binary` compares.
    auto compare = [&](ast::BinaryOp op) {
      using enum ast::BinaryOp;
      switch (cls(ot)) {
        case TypeClass::pointer:
        case TypeClass::floating: {
          const ir::Pred p = op == lt ? ir::Pred::lt : op == gt ? ir::Pred::gt : ir::Pred::eq;
          const ir::Op k = cls(ot) == TypeClass::pointer ? ir::Op::ptr_cmp : ir::Op::fcmp;
          return pure(k, b, {x, y}, {std::to_underlying(p)});
        }
        default: {
          const bool s = is_signed(ot);
          const ir::Pred p = op == lt ? (s ? ir::Pred::slt : ir::Pred::ult) : op == gt ? (s ? ir::Pred::sgt : ir::Pred::ugt)
                                      : ir::Pred::eq;
          return pure(ir::Op::icmp, b, {x, y}, {std::to_underlying(p)});
        }
      }
    };
    auto value = [&](std::size_t i) { return pure(ir::Op::global, ptr, {}, {word(results[i])}); };
    const ValueId less = compare(ast::BinaryOp::lt), equal = compare(ast::BinaryOp::eq);
    ValueId otherwise = value(2);
    if (results.size() > 3) {
      // Ordered: false when either operand is a NaN ([expr.rel]), which leaves `unordered`.
      otherwise = pure(ir::Op::select, ptr, {compare(ast::BinaryOp::gt), value(2), value(3)});
    }
    const ValueId source = pure(ir::Op::select, ptr, {less, value(0), pure(ir::Op::select, ptr, {equal, value(1), otherwise})});
    copy(p, {source, p.type});
  }

  // Initializes the object at `p` from the prvalue `e` ([dcl.init]). In a `zeroed` object, a
  // value-initialized element is already zero.
  void initialize(const Place& p, At e, bool zeroed, bool select_class = false) {
    Position here(*this, e.offset);
    if (is<ast::Paren>(e)) return initialize(p, child(e, get<ast::Paren>(e).operand), zeroed, select_class);
    if (is<ast::DefaultArg>(e))
      return pinned([&] { initialize(p, child(e, get<ast::DefaultArg>(e).arg), zeroed, select_class); });
    if (is<ast::DefaultSourceContext>(e))
      return with_default_source(e, [&](At arg) { initialize(p, arg, zeroed, select_class); });
    if (facts_.is_reference(p.type)) {
      store(p, address(e));  // a reference member holds its object's address
      return;
    }
    if (is<ast::Literal>(e)) {
      const auto literal = get<ast::Literal>(e);
      if (in_.values.is<ast::VAggregate>(literal.value)) return initialize_constant(p, literal.value);
    }
    if (select_class && class_selection(e)) return initialize_selection(p, e, zeroed);
    if (is<ast::ImplicitConversion>(e)) {
      auto c = get<ast::ImplicitConversion>(e);
      if (c.conversion == ast::ConversionKind::array_copy)
        return copy_array(p, lvalue(child(e, c.operand)));
    }
    if (is<ast::Call>(e) && class_prvalue(info(e))) {
      call(e, p.ptr);  // the callee initializes the object ([stmt.return])
      return;
    }
    if (member_call_expression(e) && class_prvalue(info(e))) {
      member_call(e, p.ptr);
      return;
    }
    if (is<ast::CtorCall>(e)) return call_constructor(p.ptr, e, zeroed);
    if (is<ast::ThreeWay>(e)) return three_way(p, e);
    if (is<ast::BitCast>(e) && cls(p.type) == TypeClass::record) {
      bit_cast(e, p);
      return;
    }
    if (is<ast::InitList>(e)) {
      bool array = cls(p.type) == TypeClass::array;
      // A class's elements are its direct bases, then its members ([dcl.init.aggr]/2).
      ast::EntityId owner = array ? ast::EntityId::none : facts_.class_entity(p.type);
      std::vector<ast::EntityId> bases = array ? std::vector<ast::EntityId>{} : facts_.bases_of(owner);
      uint32_t i = 0;
      for (ast::Child c : ast_.children(get<ast::InitList>(e).elems)) {
        At x = child(e, c);
        if (i < bases.size()) {
          initialize(subobject(p, ir::Step::base, std::to_underlying(bases[i]), info(x).type), x, zeroed);
        } else {
          uint32_t index = i - uint32_t(bases.size());
          ast::QualType type = info(x).type;
          // A reference initializer is a glvalue of the referent's type. The destination
          // field's reference type tells initialize to store its address ([dcl.init.ref]).
          if (!array)
            if (auto field = facts_.field_type(owner, index); field && facts_.is_reference(*field)) type = *field;
          Place destination = subobject(p, array ? ir::Step::elem : ir::Step::field, index, type);
          const bool select_element = select_class && (array || !facts_.potentially_overlapping_field(owner, index));
          if (is<ast::DefaultMemberInit>(x)) {
            Position member_location(*this, x.offset);
            // Only a replayed default uses the destination's `this`. An explicit clause can
            // still refer to the enclosing member function's own object.
            ValueId receiver = p.ptr;
            if (p.type != ast::without_cv(p.type)) {
              ast::QualType pointer = facts_.pointer_to(ast::without_cv(p.type));
              receiver = pure(ir::Op::ptr_cast, pointer, {receiver}, {word(ty(pointer))});
            }
            struct RestoreDefault {
              Gen& gen;
              std::optional<ValueId> receiver;
              bool in_default;
              ~RestoreDefault() {
                gen.this_ = receiver;
                gen.in_aggregate_default_ = in_default;
              }
            } restore{*this, std::exchange(this_, receiver), std::exchange(in_aggregate_default_, true)};
            pinned([&] {
              initialize(initializing_place(destination), child(x, get<ast::DefaultMemberInit>(x).init), false, select_element);
            });
          } else {
            initialize(destination, x, zeroed, select_element);
          }
          // [basic.life], [dcl.init.aggr]: a later default can read an earlier initialized
          // scalar member before initialization of the complete aggregate finishes.
          if (!array && bases.empty() && !destination.bit_field) {
            TypeClass kind = cls(type);
            if (kind == TypeClass::bool_ || kind == TypeClass::integer) begin_lifetime(destination);
          }
        }
        ++i;
      }
      return;
    }
    if (is<ast::Lambda>(e)) {
      // A closure object: each capture's member initialized in order ([expr.prim.lambda.capture]/15).
      for (ast::Child c : ast_.children(get<ast::Lambda>(e).inits)) {
        At x = child(e, c);
        auto m = get<ast::MemberInit>(x);
        initialize(subobject(p, ir::Step::field, m.field, m.type), child(x, m.init), zeroed);
      }
      return;
    }
    if (is<ast::DefaultInit>(e)) return;
    if (is<ast::ValueInit>(e)) {
      if (zeroed) return;
      if (cls(p.type) == TypeClass::record || cls(p.type) == TypeClass::array) zero_fill(p);
      else store(p, zero(p.type));
      return;
    }
    if (cls(p.type) == TypeClass::record || cls(p.type) == TypeClass::array) {
      copy(p, class_source(e));
      return;
    }
    store(p, rvalue(e));
  }

  // An argument: a glvalue binds a reference parameter, so it passes its object's address.
  ValueId argument(At a) { return is_lvalue(a) ? address(a) : rvalue(a); }

  // A temporary ([class.temporary]): initialized from a prvalue, ended with the full-expression
  // or, when a reference it is bound to extends it, with that reference's scope.
  Place materialize_value(At site, At value, ast::QualType type,
                          ast::EntityId destructor = ast::EntityId::none,
                          uint32_t extended = 0, bool select_class = false) {
    Position here(*this, site.offset);
    if (extended == 1 && (selecting_initializer_ || branches_ > 0))
      unsupported(site, "requiring lifetime extension in a conditional operand or class initializer selection");
    bool zeroed = value_initializes(value);
    Place p = begin_object(type, zeroed ? ir::Init::zero : ir::Init::indeterminate, ir::Storage::temporary);
    initialize(p, value, zeroed, select_class);
    begin_lifetime(p);
    Ending end{p, destructor, ir::Storage::temporary};
    if (extended == 1) scopes_.back().push_back(end);
    else if (temps_.empty()) unsupported(site, "outside a full-expression");
    else if (branches_ > 0 && destructor != ast::EntityId::none) unsupported(site, "with a destructor, created conditionally");
    else temps_.back().push_back(end);
    return p;
  }

  Place materialize(At a, bool select_class = false) {
    const auto m = get<ast::Materialize>(a);
    const At value = child(a, m.operand);
    return materialize_value(a, value, m.info.type, m.destructor, m.extended,
                             select_class || has_bit_cast_result(value));
  }

  // The object a class prvalue is copied from: a glvalue of the class converted to a prvalue
  // ([conv.lval]), whose copy constructor is trivial.
  Place class_source(At e) {
    while (is<ast::Paren>(e) || is<ast::DefaultArg>(e))
      e = child(e, is<ast::Paren>(e) ? get<ast::Paren>(e).operand : get<ast::DefaultArg>(e).arg);
    if (is<ast::DefaultSourceContext>(e)) return with_default_source(e, [&](At arg) { return class_source(arg); });
    if (is<ast::ImplicitConversion>(e)) {
      auto c = get<ast::ImplicitConversion>(e);
      if (c.conversion == ast::ConversionKind::lvalue_to_rvalue) return lvalue(child(e, c.operand));
    }
    unsupported(e, "as a class prvalue");
  }
  void copy(const Place& dst, const Place& src) {
    ValueId ops[] = {dst.ptr, src.ptr};
    std::uint64_t imms[] = {word(ty(dst.type))};
    stateful(ir::Op::copy, {}, ops, imms);
  }
  // Zero-initializes ([dcl.init.general]) an object whose storage was not allocated zero: copies
  // the representation of one that is.
  void zero_fill(const Place& p) {
    const Place zeros = begin_object(p.type, ir::Init::zero, ir::Storage::temporary);
    begin_lifetime(zeros);
    copy(p, zeros);
    ValueId ops[] = {zeros.ptr};
    stateful(ir::Op::obj__end, {}, ops);
    std::uint64_t imms[] = {std::to_underlying(ir::Storage::temporary)};
    stateful(ir::Op::dealloc, {}, ops, imms);
  }
  ValueId zero(ast::QualType t) {
    if (in_.types.is<ast::TMemberPtr>(in_.types.canon(t))) throw Unsupported{"value-initializing a pointer to member"};
    if (cls(t) == TypeClass::pointer || is_nullptr(t)) return constant(t, facts_.null_constant(t));
    if (cls(t) == TypeClass::floating) return float_constant(t, 0.0);
    return int_constant(t, 0);
  }

  void if_(At a) {
    auto s = get<ast::If>(a);
    push_scope();
    stmt(child(a, s.init));
    ValueId c = condition(child(a, s.condition));
    ir::BlockId then_b = new_block(), join = new_block();
    ir::BlockId else_b = s.else_.id != NodeId::none ? new_block() : join;
    cond_br(c, edge(then_b), edge(else_b));
    enter(then_b);
    substmt(child(a, s.then_));
    if (live_) br(join);
    if (s.else_.id != NodeId::none) {
      enter(else_b);
      substmt(child(a, s.else_));
      if (live_) br(join);
    }
    enter(join);
    pop_scope();
  }

  // [stmt.if]: the first substatement on `consteval_br`'s consteval edge, the second on its
  // runtime edge; `if !consteval` swaps them.
  void if_consteval(At a) {
    auto s = get<ast::IfConsteval>(a);
    ir::BlockId then_b = new_block(), join = new_block();
    ir::BlockId else_b = s.else_.id != NodeId::none ? new_block() : join;
    ir::SuccRef succs[] = {edge(s.negated ? else_b : then_b), edge(s.negated ? then_b : else_b)};
    emit(ir::Op::consteval_br, {}, {}, {}, ir::bit(ir::Flag::observes_context), succs);
    live_ = false;
    enter(then_b);
    substmt(child(a, s.then_));
    if (live_) br(join);
    if (s.else_.id != NodeId::none) {
      enter(else_b);
      substmt(child(a, s.else_));
      if (live_) br(join);
    }
    enter(join);
  }

  // A condition's value ([stmt.pre]); a variable it declares begins in the current scope first.
  ValueId condition(At c) {
    if (is<ast::CondDecl>(c)) {
      auto d = get<ast::CondDecl>(c);
      {
        Position at(*this, child(c, d.var).offset);
        local_var(child(c, d.var));
      }
      return full_expression([&] { return rvalue(child(c, d.value)); });
    }
    return full_expression([&] { return rvalue(c); });
  }

  // [basic.start.dynamic]: the variable's object is initialized, then its destructor is registered
  // with `__cxa_atexit(destructor, &object, &__dso_handle)` ([basic.start.term]).
  void static_init(At a) {
    auto si = get<ast::StaticInit>(a);
    Place p{pure(ir::Op::global, facts_.pointer_to(si.type), {}, {word(si.variable)}), si.type};
    if (si.init.id != NodeId::none) {
      if (facts_.is_reference(si.type) && materializes_temporary(ast_, si.init.id))
        throw Unsupported{"a materialized temporary in a namespace-scope reference initializer"};
      pinned([&] {
        full_expression([&] {
          initialize(p, child(a, si.init), false);
          return 0;
        });
      });
    }
    if (si.destructor != ast::EntityId::none) at_exit(p, si.destructor, si.atexit, si.dso);
  }
  // Registers `destructor` to run on the object at `p` at exit with `__cxa_atexit(destructor,
  // object, &__dso_handle)` ([basic.start.term]).
  void at_exit(const Place& p, ast::EntityId destructor, ast::EntityId atexit_fn, ast::EntityId dso) {
    ast::QualType vp = facts_.pointer_to(facts_.int_type());  // any object pointer: the callee takes void*
    auto atexit = [&](ValueId object) {
      ValueId args[] = {pure(ir::Op::func_addr, vp, {}, {word(destructor)}), object, pure(ir::Op::global, vp, {}, {word(dso)})};
      call_fn(atexit_fn, facts_.int_type(), args);
    };
    if (cls(p.type) != TypeClass::array) return atexit(p.ptr);
    // An array's elements are each registered, the first first, so that the last is destroyed
    // first: a loop over the innermost elements, from the first.
    Place first = p;
    std::uint64_t n = 1;
    while (cls(first.type) == TypeClass::array) {
      n *= facts_.bound(first.type);
      first = subobject(first, ir::Step::elem, 0, facts_.element(first.type));
    }
    ast::QualType it = facts_.ptrdiff_type();
    ir::BlockId head = new_block({it}), body = new_block(), done = new_block();
    br(head, {int_constant(it, 0)});
    enter(head);
    ValueId i = b_.param(head, 1);
    cond_br(pure(ir::Op::icmp, facts_.bool_type(), {i, int_constant(it, std::int64_t(n))}, {std::to_underlying(ir::Pred::slt)}),
            edge(body), edge(done));
    enter(body);
    atexit(pure(ir::Op::ptr_add, facts_.pointer_to(first.type), {first.ptr, i}, {word(ty(first.type))}));
    br(head, {arith(ast::BinaryOp::add, it, i, int_constant(it, 1))});
    enter(done);
  }

  // The entity identifies a static local independently of the declaration's control flow
  // ([stmt.dcl]/3). Form its address where it is needed so a skipped declaration and its reads
  // do not require the interpreter to materialize the object.
  Place static_place(const ast::LocalRec& local) {
    return {pure(ir::Op::global, facts_.pointer_to(local.type), {}, {word(local.entity)}), local.type};
  }

  // [stmt.dcl]/3: a static local's object is its entity's. Unless constant-initialized with
  // nothing to destroy, it is initialized the first time control passes here, once even with
  // several threads: `if (__cxa_guard_acquire(&guard)) { init; atexit(destructor); __cxa_guard_release(&guard); }`
  // (Itanium C++ ABI §3.3.3), the acquire called on every pass.
  void static_var(At a) {
    auto s = get<ast::StaticVar>(a);
    const ast::LocalRec rec = in_.seqs.at(fn_.body.locals, s.local);
    const bool needs_guard = s.destructor != ast::EntityId::none ||
                             (s.value == ast::ConstId::none && s.init.id != NodeId::none);
    const auto g = needs_guard ? std::ranges::find(fn_.guards, rec.entity, &FunctionInput::StaticGuard::variable)
                               : fn_.guards.end();
    if (needs_guard && (g == fn_.guards.end() || fn_.guard_acquire == ast::EntityId::none)) {
      // Constant evaluation has no guard or exit registration. A usable object is already
      // constant-initialized. Otherwise, passing the declaration is not constant ([expr.const]).
      if (s.usable_in_constant_expressions) return;
      emit(ir::Op::unreachable, {}, {}, {}, ir::bit(ir::Flag::not_constexpr));
      live_ = false;
      return;
    }
    if (!s.usable_in_constant_expressions) {
      // The declaration is forbidden on an evaluated constant path even without a read.
      ir::BlockId invalid = new_block(), runtime = new_block();
      ir::SuccRef succs[] = {edge(invalid), edge(runtime)};
      emit(ir::Op::consteval_br, {}, {}, {}, 0, succs);
      enter(invalid);
      emit(ir::Op::unreachable, {}, {}, {}, ir::bit(ir::Flag::not_constexpr));
      enter(runtime);
    }
    if (!needs_guard) return;
    ast::QualType vp = facts_.pointer_to(facts_.int_type());
    ValueId guard[] = {pure(ir::Op::global, vp, {}, {word(g->guard)})};
    ValueId first = *call_fn(fn_.guard_acquire, facts_.int_type(), guard);
    ValueId taken = pure(ir::Op::icmp, facts_.bool_type(), {first, int_constant(facts_.int_type(), 0)}, {std::to_underlying(ir::Pred::ne)});
    ir::BlockId init_b = new_block(), join = new_block();
    cond_br(taken, edge(init_b), edge(join));
    enter(init_b);
    const Place p = static_place(rec);
    if (s.init.id != NodeId::none && s.value == ast::ConstId::none)
      full_expression([&] {
        initialize(p, child(a, s.init), false);
        return 0;
      });
    if (s.destructor != ast::EntityId::none) at_exit(p, s.destructor, fn_.atexit, fn_.dso);
    call_fn(fn_.guard_release, facts_.int_type(), guard, ImplicitObject::none, true);
    br(join);
    enter(join);
  }

  struct Loop {
    ir::BlockId next, exit;     // where `continue` and `break` go
    std::size_t depth;          // the scopes `break` leaves open
    std::size_t continue_depth; // the scopes `continue` leaves open
    bool in_loop = true;        // false for a switch outside every loop: no `continue`
  };

  // `while`, `do` and `for` ([stmt.iter]): the condition block, the body, then for `for` the
  // increment block, which `continue` reaches. A variable the condition declares lives for one
  // iteration ([stmt.while]/2), through the increment ([stmt.for]/1): `break` and a false
  // condition end it, `continue` goes to the block that ends it.
  void loop(At a, ast::Child init, ast::Child cond, ast::Child incr, ast::Child body, bool body_first) {
    push_scope();  // a for-init-statement's scope ([stmt.for])
    if (init.id != NodeId::none) stmt(child(a, init));
    bool decl = cond.id != NodeId::none && is<ast::CondDecl>(child(a, cond));
    ir::BlockId head = new_block(), body_b = new_block(), exit = new_block();
    ir::BlockId next = incr.id != NodeId::none || decl ? new_block() : head;
    br(body_first ? body_b : head);
    enter(head);
    std::size_t outer = scopes_.size();
    if (decl) push_scope();
    if (cond.id != NodeId::none) {
      ValueId c = condition(child(a, cond));
      ir::BlockId out = decl ? new_block() : exit;
      cond_br(c, edge(body_b), edge(out));
      if (decl) {
        enter(out);
        exit_scopes(outer);
        br(exit);
      }
    } else {
      br(body_b);
    }
    enter(body_b);
    loops_.push_back({next, exit, outer, scopes_.size()});
    substmt(child(a, body));
    loops_.pop_back();
    if (live_) br(next);
    if (next != head) {
      enter(next);
      if (incr.id != NodeId::none)
        full_expression([&] {
          discard(child(a, incr));
          return 0;
        });
      exit_scopes(outer);
      br(head);
    }
    if (decl) drop_scope();
    enter(exit);
    pop_scope();
  }

  // [stmt.switch]: the condition is compared with each case value in turn, then control goes to
  // `default`, or past the statement. The case labels are found in the body first, since the
  // comparisons come before it.
  struct Cases {
    std::size_t depth;  // the scopes open at the jump
    std::vector<std::pair<At, ir::BlockId>> blocks;
  };
  void switch_(At a) {
    auto s = get<ast::Switch>(a);
    push_scope();
    stmt(child(a, s.init));
    At cond = child(a, s.condition);
    ast::QualType t = is<ast::CondDecl>(cond) ? info(child(cond, get<ast::CondDecl>(cond).value)).type : info(cond).type;
    ValueId c = condition(cond);
    std::vector<At> labels;
    case_labels(child(a, s.body), labels);
    ir::BlockId exit = new_block(), dflt = exit;
    Cases cs{scopes_.size(), {}};
    for (At l : labels) {
      cs.blocks.push_back({l, new_block()});
      if (is<ast::Default>(l)) dflt = cs.blocks.back().second;
    }
    for (auto& [l, b] : cs.blocks) {
      if (!is<ast::Case>(l)) continue;
      ValueId eq = pure(ir::Op::icmp, facts_.bool_type(), {c, constant(t, get<ast::Case>(l).value)},
                        {std::to_underlying(ir::Pred::eq)});
      ir::BlockId more = new_block({t});
      cond_br(eq, edge(b), edge(more, {c}));
      enter(more);
      c = b_.param(more, 1);
    }
    br(dflt);
    cases_.push_back(std::move(cs));
    if (loops_.empty()) loops_.push_back({exit, exit, scopes_.size(), scopes_.size(), false});
    else loops_.push_back({loops_.back().next, exit, scopes_.size(), loops_.back().continue_depth, loops_.back().in_loop});
    substmt(child(a, s.body));
    loops_.pop_back();
    cases_.pop_back();
    if (live_) br(exit);
    enter(exit);
    pop_scope();
  }
  // The case and default labels of a switch's body, in order, outside nested switches.
  void case_labels(At a, std::vector<At>& out) {
    if (a.id == NodeId::none) return;
    auto sub = [&](ast::Child c) { case_labels(child(a, c), out); };
    if (is<ast::Compound>(a)) {
      for (ast::Child c : ast_.children(get<ast::Compound>(a).stmts)) sub(c);
    } else if (is<ast::Case>(a)) {
      out.push_back(a);
      sub(get<ast::Case>(a).stmt);
    } else if (is<ast::Default>(a)) {
      out.push_back(a);
      sub(get<ast::Default>(a).stmt);
    } else if (is<ast::Label>(a)) {
      sub(get<ast::Label>(a).stmt);
    } else if (is<ast::If>(a)) {
      sub(get<ast::If>(a).then_);
      sub(get<ast::If>(a).else_);
    } else if (is<ast::IfConsteval>(a)) {
      sub(get<ast::IfConsteval>(a).then_);
      sub(get<ast::IfConsteval>(a).else_);
    } else if (is<ast::While>(a)) {
      sub(get<ast::While>(a).body);
    } else if (is<ast::Do>(a)) {
      sub(get<ast::Do>(a).body);
    } else if (is<ast::For>(a)) {
      sub(get<ast::For>(a).body);
    }
  }
  void case_label(At a) {
    if (cases_.empty()) unsupported(a, "outside a switch");
    const Cases& cs = cases_.back();
    auto b = std::ranges::find_if(cs.blocks, [&](const auto& p) { return p.first.id == a.id && p.first.offset == a.offset; });
    if (b == cs.blocks.end()) unsupported(a, "not found in its switch");
    auto entered = std::span(scopes_).subspan(cs.depth);
    ir::BlockId target = b->second;
    if (std::ranges::any_of(entered, [](const auto& scope) { return !scope.empty(); })) {
      target = new_block();
      if (live_) br(target);
      enter(b->second);
      for (const auto& scope : entered)
        for (const Ending& e : scope) begin_bypassed(a, e, ir::bit(ir::Flag::switch_bypassed_lifetime));
      br(target);
    } else if (live_) {
      br(target);
    }
    enter(target);
    stmt(child(a, is<ast::Case>(a) ? get<ast::Case>(a).stmt : get<ast::Default>(a).stmt));
  }

  // Labels and `goto` ([stmt.label], [stmt.goto]). A jump ends the objects of the scopes it
  // leaves, and, in the scopes it stays in, those begun after the label ([stmt.jump]/2). A goto
  // before its label branches to a block of its own, filled once the label is placed.
  struct LabelInfo {
    ir::BlockId block;
    bool placed = false;
    std::vector<std::uint32_t> path;  // the ids of the scopes open at the label, outermost first
    std::vector<std::vector<Ending>> scopes;  // the objects active at the label
  };
  struct PendingGoto {
    At at;
    ast::NameId label;
    ir::BlockId from;
    std::vector<std::uint32_t> path;
    std::vector<std::vector<Ending>> scopes;
    uint32_t loc;
  };
  LabelInfo& label_info(ast::NameId n) {
    auto [it, fresh] = labels_.try_emplace(n);
    if (fresh) it->second.block = new_block();
    return it->second;
  }
  void jump_scopes(At a, std::span<const std::uint32_t> path, std::span<const std::vector<Ending>> scopes, const LabelInfo& l) {
    std::size_t k = 0;
    while (k < path.size() && k < l.path.size() && path[k] == l.path[k]) ++k;
    for (std::size_t i = scopes.size(); i-- > 0;)
      for (std::size_t j = scopes[i].size(); j-- > (i < k ? l.scopes[i].size() : 0);) end_lifetime(scopes[i][j]);
    for (std::size_t i = 0; i < l.scopes.size(); ++i)
      for (std::size_t j = i < k ? scopes[i].size() : 0; j < l.scopes[i].size(); ++j)
        begin_bypassed(a, l.scopes[i][j]);
  }
  void goto_(At a) {
    auto g = get<ast::Goto>(a);
    LabelInfo& l = label_info(g.label);
    if (l.placed) {
      jump_scopes(a, scope_ids_, scopes_, l);
      br(l.block, {}, ir::bit(ir::Flag::not_constexpr));  // a goto is never constant ([expr.const])
      return;
    }
    ir::BlockId from = new_block();
    br(from, {}, ir::bit(ir::Flag::not_constexpr));
    gotos_.push_back({a, g.label, from, scope_ids_, scopes_, loc_});
  }
  void label(At a) {
    auto s = get<ast::Label>(a);
    LabelInfo& l = label_info(s.name);
    if (live_) br(l.block);
    enter(l.block);
    l.placed = true;
    l.path = scope_ids_;
    l.scopes = scopes_;
    ValueId state = state_;
    for (auto g = gotos_.begin(); g != gotos_.end();) {
      if (g->label != s.name) {
        ++g;
        continue;
      }
      enter(g->from);
      Position at(*this, g->loc);
      jump_scopes(g->at, g->path, g->scopes, l);
      br(l.block);
      g = gotos_.erase(g);
    }
    block_ = l.block;
    state_ = state;
    live_ = true;
    stmt(child(a, s.stmt));
  }

  // ------------------------------------------------------------------ expressions

  ast::ExprInfo info(At a) {
    if (a.id == NodeId::none) throw Unsupported{"an initializer that failed to be analysed"};
    ast::ExprInfo r{};
    ast::dispatch<ast::Nodes>(ast::kind_of(a.id), [&]<class T>(std::type_identity<T>) {
      if constexpr (requires(T t) { t.info; }) r = ast_.get<T>(a.id).info;
      else unsupported(a, "as an expression");
    });
    return r;
  }
  bool is_lvalue(At a) { return info(a).category != ast::ValueCategory::prvalue; }

  // Evaluates an expression for its side effects ([expr.context]).
  void discard(At a) {
    if (a.id == NodeId::none) return;
    // A discarded three-way comparison: only its operands' evaluations remain.
    const At written = strip_parens(a);
    if (is<ast::ThreeWay>(written)) {
      const auto tw = get<ast::ThreeWay>(written);
      unsequenced(std::array{child(written, tw.lhs), child(written, tw.rhs)},
                  [&](std::size_t, At operand) { discard(operand); });
      return;
    }
    const auto expr = info(a);
    if (expr.category == ast::ValueCategory::prvalue) {
      if (class_prvalue(expr) && has_bit_cast_result(written)) {
        materialize_value(written, written, expr.type, ast::EntityId::none, /*extended=*/0, /*select_class=*/true);
      } else {
        rvalue_or_void(a);
      }
      return;
    }
    // Discarding a nonvolatile direct field designator does not read its value.
    const auto cv = ast::cv_of(expr.type);
    if (cv == ast::Cv::none || cv == ast::Cv::const_) {
      if (default_field(a)) return;
      // In a thunk, discarding a record glvalue evaluates its effects without requiring
      // the object's address. Preserve the sequence and selected arm of compound expressions.
      if (fn_.thunk && cls(expr.type) == TypeClass::record && access_flags(expr.type) == 0) {
        const At operand = strip_parens(a);
        if (is<ast::OrderedBinary>(operand)) {
          const auto sequence = get<ast::OrderedBinary>(operand);
          if (sequence.op == ast::OrderedOp::comma) {
            discard(child(operand, sequence.lhs));
            discard(child(operand, sequence.rhs));
            return;
          }
        }
        if (is<ast::Conditional>(operand)) {
          conditional(operand, /*glvalue=*/false, ast::builtin(facts_.interners(), ast::Builtin::void_));
          return;
        }
        if (is<ast::DeclRef>(operand) && !get<ast::DeclRef>(operand).reference) return;
        if (is<ast::LocalRef>(operand)) {
          const auto local = get<ast::LocalRef>(operand).local;
          // Naming a reference parameter need not observe the referent when the result is
          // discarded ([expr.const.core]). An actual use still needs its binding.
          if (!facts_.is_reference(in_.seqs.at(fn_.body.locals, local).type) ||
              std::ranges::binary_search(fn_.discardable_reference_parameters, local))
            return;
        }
      }
    }
    lvalue(a);
  }

  ValueId rvalue(At a) {
    auto v = rvalue_or_void(a);
    if (!v) unsupported(a, "of type void used as a value");
    return *v;
  }

  // How much an expression touches memory, for unsequenced operands: 0 not at all, 1 reads, 2
  // writes. A call counts by its arguments only: what the callee does is indeterminately sequenced
  // with the caller's other evaluations, not unsequenced ([intro.execution]), so it is outside
  // any `useq` group.
  int effects(At a) {
    if (a.id == NodeId::none || is<ast::ConstantProbe>(a)) return 0;
    if (is<ast::Assign>(a) || is<ast::ExplicitDestroy>(a)) return 2;
    if (is<ast::VariadicBuiltin>(a) && get<ast::VariadicBuiltin>(a).op != ast::VaOp::end) return 2;
    if (is<ast::Unary>(a)) {
      auto op = get<ast::Unary>(a).op;
      if (op == ast::UnaryOp::pre_inc || op == ast::UnaryOp::pre_dec || op == ast::UnaryOp::post_inc ||
          op == ast::UnaryOp::post_dec)
        return 2;
    }
    int e = is<ast::DynamicCastVoid>(a) || is<ast::DynamicCastPointer>(a) ? 1 : 0;
    if (is<ast::BitCast>(a) && is_lvalue(child(a, get<ast::BitCast>(a).source))) e = 1;
    if (is<ast::ImplicitConversion>(a)) {
      auto c = get<ast::ImplicitConversion>(a);
      if (c.conversion == ast::ConversionKind::lvalue_to_rvalue && !is_nullptr(c.info.type)) e = 1;
    }
    ast::Red(ast_, a.id).for_each_child([&](const ast::Red& c) { e = std::max(e, effects({c.id(), 0})); });
    return e;
  }

  // Evaluates unsequenced operands in order, inside a `useq` group when they can conflict.
  template <std::size_t N, class Each>
  void unsequenced(std::array<At, N> operands, Each&& each) {
    int writers = 0, touchers = 0;
    for (At o : operands) {
      int e = effects(o);
      writers += e == 2;
      touchers += e >= 1;
    }
    bool group = writers >= 1 && touchers >= 2;
    if (group) stateful(ir::Op::useq__begin, {}, {});
    for (std::size_t i = 0; i < N; ++i) {
      if (group && i) stateful(ir::Op::useq__next, {}, {});
      each(i, operands[i]);
    }
    if (group) stateful(ir::Op::useq__end, {}, {});
  }

  std::optional<ValueId> rvalue_or_void(At a) {
    Position here(*this, a.offset);
    ast::ExprInfo ei = info(a);
    if (ei.category != ast::ValueCategory::prvalue) unsupported(a, "as a glvalue where a prvalue is expected");
    ast::QualType t = ei.type;
    if (in_.types.is<ast::TMemberPtr>(in_.types.canon(t))) unsupported(a, "with a pointer-to-member value");
    if (is<ast::Literal>(a)) return constant(t, get<ast::Literal>(a).value);
    if (is<ast::ConstantProbe>(a)) return int_constant(t, get<ast::ConstantProbe>(a).proved);
    if (is<ast::BitCast>(a)) return bit_cast(a);
    if (is<ast::SourceLine>(a))
      return int_constant(t, source_line_ != 0 ? source_line_ : get<ast::SourceLine>(a).lexical_line);
    if (is<ast::SourceLocation>(a)) {
      if (source_descriptor_ == ast::ConstId::none) unsupported(a, "without a bound source location");
      return constant(t, source_descriptor_);
    }
    if (is<ast::Paren>(a)) return rvalue_or_void(child(a, get<ast::Paren>(a).operand));
    if (is<ast::DefaultArg>(a)) return pinned([&] { return rvalue_or_void(child(a, get<ast::DefaultArg>(a).arg)); });
    if (is<ast::DefaultSourceContext>(a)) return with_default_source(a, [&](At arg) { return rvalue_or_void(arg); });
    if (is<ast::ImplicitConversion>(a)) return conversion(a);
    if (is<ast::DynamicCastVoid>(a)) return dynamic_cast_void(a);
    if (is<ast::DynamicCastPointer>(a)) return dynamic_cast_pointer(a);
    if (is<ast::Unary>(a)) return unary(a);
    if (is<ast::Binary>(a)) return binary(a);
    if (is<ast::OrderedBinary>(a)) return ordered(a);
    if (is<ast::NewObject>(a)) return new_object(a);
    if (is<ast::DeleteObject>(a)) {
      delete_object(a);
      return std::nullopt;
    }
    if (is<ast::NewArray>(a)) return new_array(a);
    if (is<ast::DeleteArray>(a)) {
      delete_array(a);
      return std::nullopt;
    }
    if (is<ast::ExplicitDestroy>(a)) {
      const auto d = get<ast::ExplicitDestroy>(a);
      const At object = child(a, d.object);
      const ValueId address = rvalue(object);
      if (d.function != ast::EntityId::none) destroy({address, facts_.pointee(info(object).type)}, d.function);
      stateful(ir::Op::obj__end, {}, std::span(&address, 1));
      return std::nullopt;
    }
    if (is<ast::Call>(a)) return call(a);
    if (member_call_expression(a)) return member_call(a);
    if (is<ast::VariadicBuiltin>(a)) return variadic_builtin(a);
    if (is<ast::AtomicCall>(a)) return atomic_builtin(a);
    if (is<ast::Throw>(a)) {
      if (selecting_initializer_) unsupported(a, "inside a class initializer selection");
      auto expr = get<ast::Throw>(a);
      if (expr.operand.id == NodeId::none) {
        ValueId ops[] = {state_};
        emit(ir::Op::rethrow, {}, ops);
      } else {
        At operand = child(a, expr.operand);
        const auto exception_type = ast::without_cv(ty(info(operand).type));
        ValueId value;
        if (expr.exception_class == ast::EntityId::none) {
          value = rvalue(operand);
        } else {
          // Stage the trivial representation before entering the runtime. If the initializer
          // throws, there is no allocated exception object to release.
          const Place payload = begin_object(exception_type, ir::Init::indeterminate, ir::Storage::temporary);
          initialize(payload, operand, false);
          begin_lifetime(payload);
          value = payload.ptr;
        }
        ValueId ops[] = {state_, value};
        std::uint64_t type[] = {word(exception_type)};
        emit(ir::Op::throw_, {}, ops, type);
      }
      live_ = false;
      ensure_live();
      return std::nullopt;
    }
    if (is<ast::BuiltinCall>(a)) {
      auto b = get<ast::BuiltinCall>(a);
      if (b.op == ast::BuiltinOp::trap || b.op == ast::BuiltinOp::unreachable) {
        // Native code traps on both; constant evaluation names which was reached ([expr.const]).
        const auto flag = b.op == ast::BuiltinOp::trap ? ir::Flag::explicit_trap : ir::Flag::assumed_unreachable;
        emit(ir::Op::unreachable, {}, {}, {}, ir::bit(flag));
        live_ = false;
        // A parent expression can still emit its remaining operands or join edge.
        ensure_live();
        return std::nullopt;
      }
      const auto args = ast_.children(b.args);
      switch (b.op) {
        case ast::BuiltinOp::fabs: return pure(ir::Op::fabs, t, {rvalue(child(a, args[0]))});
        case ast::BuiltinOp::ctz:
        case ast::BuiltinOp::ctz_gcc_zero:
        case ast::BuiltinOp::clzll: {
          const auto op = b.op == ast::BuiltinOp::clzll ? ir::Op::clz : ir::Op::ctz;
          const auto flags = b.op == ast::BuiltinOp::ctz ? ir::Flags{0} : ir::bit(ir::Flag::count_zero_gcc);
          return pure(op, t, {rvalue(child(a, args[0]))}, {}, flags);
        }
        case ast::BuiltinOp::nansl: {
          const ir::Ty result[] = {vty(t)};
          const std::uint64_t fn[] = {word(ir::BuiltinFn::nansl)};
          return b_.result(stateful(ir::Op::builtin, result, {}, fn), 1);
        }
        case ast::BuiltinOp::strlen: {
          const At arg = child(a, args[0]);
          return string_length(t, info(arg).type, rvalue(arg));
        }
        case ast::BuiltinOp::add_overflow:
        case ast::BuiltinOp::mul_overflow: {
          const At left = child(a, args[0]);
          const ValueId operands[] = {rvalue(left), rvalue(child(a, args[1]))};
          const ValueId destination = rvalue(child(a, args[2]));
          const ast::QualType integer = info(left).type;
          const ir::Ty results[] = {vty(integer), vty(t)};
          const auto op = b.op == ast::BuiltinOp::add_overflow ? ir::Op::sadd_overflow : ir::Op::smul_overflow;
          const ir::NodeId checked = emit(op, results, operands);
          store({destination, integer}, b_.result(checked), ir::bit(ir::Flag::call_body));
          return b_.result(checked, 1);
        }
        case ast::BuiltinOp::trap:
        case ast::BuiltinOp::unreachable: std::unreachable();
      }
      std::unreachable();
    }
    if (is<ast::This>(a)) {
      if (!this_) unsupported(a, "outside a non-static member function");
      return *this_;
    }
    if (is<ast::Conditional>(a)) return conditional(a, false).value;
    if (is<ast::ValueInit>(a) && cls(t) != TypeClass::record && cls(t) != TypeClass::array) return zero(t);
    unsupported(a);
  }

  std::optional<ValueId> atomic_builtin(At a) {
    const auto operation = get<ast::AtomicCall>(a);
    const auto args = ast_.children(operation.args);
    std::array<ValueId, 6> values;
    for (std::size_t i = 0; i < args.size(); ++i) values[i] = rvalue(child(a, args[i]));
    const std::uint64_t order[]{operation.order};
    if (operation.op == ast::AtomicOp::fence) {
      stateful(ir::Op::fence, {}, {}, order);
      return std::nullopt;
    }

    const ast::QualType object = facts_.pointee(info(child(a, args[0])).type);
    const auto integer = ast::without_cv(ty(object));
    const ir::Flags flags = access_flags(object);
    const ir::Ty word_result[]{vty(integer)};
    const std::uint64_t access[]{word(integer), operation.order};
    switch (operation.op) {
      case ast::AtomicOp::load:
        return b_.result(stateful(ir::Op::atomic__load, word_result, std::span(values).first(1), access, flags), 1);
      case ast::AtomicOp::store:
        stateful(ir::Op::atomic__store, {}, std::span(values).first(2), access, flags);
        return std::nullopt;
      case ast::AtomicOp::compare_exchange: {
        const Place expected{values[1], facts_.pointee(info(child(a, args[1])).type)};
        const ValueId compare = load(expected, ir::bit(ir::Flag::call_body));
        const ValueId operands[]{values[0], compare, values[2]};
        const ir::Ty results[]{vty(integer), vty(facts_.bool_type())};
        const std::uint64_t orders[]{word(integer), operation.order, operation.failure_order};
        const auto cas = stateful(ir::Op::atomic__cmpxchg, results, operands, orders, flags);
        const auto success = b_.result(cas, 2);
        const auto miss = new_block(), done = new_block();
        cond_br(success, edge(done), edge(miss));
        enter(miss);
        store(expected, b_.result(cas, 1), ir::bit(ir::Flag::call_body));
        br(done);
        enter(done);
        return success;
      }
      case ast::AtomicOp::rmw: {
        const std::uint64_t update[]{word(integer), operation.update, operation.order};
        const auto n = stateful(ir::Op::atomic__rmw, word_result, std::span(values).first(2), update, flags);
        const auto old = b_.result(n, 1);
        if (!operation.returns_new) return old;
        ir::Op arithmetic;
        switch (ir::Rmw(operation.update)) {
          case ir::Rmw::add: arithmetic = ir::Op::add; break;
          case ir::Rmw::sub: arithmetic = ir::Op::sub; break;
          case ir::Rmw::and_: arithmetic = ir::Op::and_; break;
          case ir::Rmw::or_: arithmetic = ir::Op::or_; break;
          case ir::Rmw::xor_: arithmetic = ir::Op::xor_; break;
          default: std::unreachable();
        }
        // The GNU operations use modular arithmetic even for signed operands.
        return pure(arithmetic, integer, {old, values[1]});
      }
      case ast::AtomicOp::fence: std::unreachable();
    }
    std::unreachable();
  }

  // Sema proved that the fields fill both representations in target byte order. Use
  // ordinary typed reads and initialization so both evaluation paths check the same objects.
  std::optional<ValueId> bit_cast(At a, std::optional<Place> into = {}) {
    const auto cast = get<ast::BitCast>(a);
    if (!cast.supported || (cls(cast.info.type) == TypeClass::record && !into))
      throw Unsupported{"evaluating __builtin_bit_cast with this object representation"};

    struct FieldValue {
      ast::QualType type;
      ValueId value;
      uint32_t bits;
    };
    std::vector<FieldValue> source;
    const At operand = child(a, cast.source);
    const auto from_type = info(operand).type;
    auto add_field = [&](ast::QualType type, ValueId value) {
      source.push_back({type, value, facts_.integer(type).width});
    };
    if (cls(from_type) == TypeClass::record) {
      const Place from = is<ast::Materialize>(operand)
          ? materialize(operand, /*select_class=*/true) : lvalue(operand);
      const auto owner = facts_.class_entity(from_type);
      for (uint32_t i = 0; const auto field = facts_.field_type(owner, i); ++i)
        add_field(*field, load(subobject(from, ir::Step::field, i, *field)));
    } else {
      add_field(from_type, is_lvalue(operand) ? load_glvalue(operand, from_type) : rvalue(operand));
    }

    // Each step consumes a contiguous slice of the current source field. Logical shifts
    // discard earlier bits, and conversion and left shift discard bits above the result field.
    std::size_t source_index = 0;
    uint32_t consumed = 0;
    auto result_field = [&](ast::QualType type) {
      const auto width = facts_.integer(type).width;
      ValueId result{};
      for (uint32_t written = 0; written < width;) {
        const auto& field = source[source_index];
        auto value = field.value;
        if (consumed != 0)
          value = shift(false, field.type, value, int_constant(facts_.int_type(), consumed));
        value = arithmetic(field.type, type, value);
        if (written == 0) {
          result = value;
        } else {
          value = shift(true, type, value, int_constant(facts_.int_type(), written));
          result = pure(ir::Op::or_, type, {result, value});
        }
        const auto count = std::min(width - written, field.bits - consumed);
        written += count;
        consumed += count;
        if (consumed == field.bits) {
          ++source_index;
          consumed = 0;
        }
      }
      return result;
    };

    if (!into) return result_field(cast.info.type);
    const auto owner = facts_.class_entity(into->type);
    for (uint32_t i = 0; const auto field = facts_.field_type(owner, i); ++i)
      store(subobject(*into, ir::Step::field, i, *field), result_field(*field));
    return std::nullopt;
  }

  ValueId dynamic_cast_void(At a) {
    const auto cast = get<ast::DynamicCastVoid>(a);
    const At operand = child(a, cast.operand);
    const ValueId object = rvalue(operand);
    const ValueId nonnull = pure(ir::Op::tobool, facts_.bool_type(), {object});
    const auto result = cast.info.type;
    const ValueId null = constant(result, facts_.null_constant(result));
    const ir::BlockId adjust = new_block({info(operand).type}), join = new_block({result});
    cond_br(nonnull, edge(adjust, {object}), edge(join, {null}));
    enter(adjust);
    ir::Ty results[] = {vty(result)};
    ValueId operands[] = {state_, b_.param(adjust, 1)};
    const ValueId most_derived = b_.result(emit(ir::Op::rtti__most_derived_addr, results, operands));
    br(join, {most_derived});
    enter(join);
    return b_.param(join, 1);
  }

  // These expressions produce null in both evaluation modes. Their operands still
  // need evaluating, for example when a call supplies a std::nullptr_t result.
  bool known_null_pointer(At a) {
    a = strip_parens(a);
    if (is<ast::ValueInit>(a)) return true;
    if (is<ast::Literal>(a)) return in_.values.is<ast::VNullptr>(get<ast::Literal>(a).value);
    if (!is<ast::ImplicitConversion>(a)) return false;
    const auto c = get<ast::ImplicitConversion>(a);
    if (c.conversion == ast::ConversionKind::null_pointer) return true;
    return (c.conversion == ast::ConversionKind::qualification || c.conversion == ast::ConversionKind::pointer) &&
           known_null_pointer(child(a, c.operand));
  }

  ValueId dynamic_cast_pointer(At a) {
    const auto cast = get<ast::DynamicCastPointer>(a);
    const At operand = child(a, cast.operand);
    const ValueId object = rvalue(operand);
    const auto result = cast.info.type;
    const ValueId null = constant(result, facts_.null_constant(result));
    if (known_null_pointer(operand)) return null;

    const ValueId nonnull = pure(ir::Op::tobool, facts_.bool_type(), {object});
    const ir::BlockId runtime = new_block({info(operand).type}), join = new_block({result});
    cond_br(nonnull, edge(runtime, {object}), edge(join, {null}));
    enter(runtime);
    auto& in = facts_.interners();
    const auto opaque = facts_.pointer_to(ast::builtin(in, ast::Builtin::void_, ast::Cv::const_));
    const auto result_pointer = facts_.pointer_to(ast::builtin(in, ast::Builtin::void_));
    const auto rtti_pointer = facts_.pointer_to(ast::builtin(in, ast::Builtin::unsigned_char));
    const auto erase_type = [&](ValueId pointer) {
      return pure(ir::Op::ptr_cast, opaque, {pointer}, {word(ty(opaque))});
    };
    const auto rtti = [&](ast::EntityId entity) {
      return erase_type(pure(ir::Op::global, rtti_pointer, {}, {word(entity)}));
    };
    // Itanium ABI §2.9.7: the source is a unique public nonvirtual base at offset zero.
    // The runtime takes the original subobject address, including secondary subobjects.
    const ValueId args[] = {erase_type(b_.param(runtime, 1)), rtti(cast.source_rtti), rtti(cast.target_rtti),
                            int_constant(facts_.ptrdiff_type(), 0)};
    const ValueId address = *call_fn(cast.runtime, result_pointer, args);
    const ValueId adjusted = pure(ir::Op::ptr_cast, result, {address}, {word(ty(result))});
    br(join, {adjusted});
    enter(join);
    return b_.param(join, 1);
  }

  ValueId string_length(ast::QualType size, ast::QualType pointer, ValueId start) {
    ast::QualType element = facts_.pointee(pointer);
    ir::BlockId head = new_block({pointer, size}), advance = new_block(), done = new_block({size});
    br(head, {start, int_constant(size, 0)});
    enter(head);
    ValueId current = b_.param(head, 1), length = b_.param(head, 2);
    ValueId byte = load({current, element}, ir::bit(ir::Flag::call_body));
    ValueId zero = pure(ir::Op::icmp, facts_.bool_type(), {byte, int_constant(element, 0)}, {word(ir::Pred::eq)});
    cond_br(zero, edge(done, {length}), edge(advance));
    enter(advance);
    ValueId one = int_constant(size, 1);
    ValueId next = pure(ir::Op::ptr_add, pointer, {current, one}, {word(ty(element))});
    br(head, {next, pure(ir::Op::add, size, {length, one})});
    enter(done);
    return b_.param(done, 1);
  }

  std::optional<ValueId> variadic_builtin(At a) {
    auto builtin = get<ast::VariadicBuiltin>(a);
    auto args = ast_.children(builtin.args);
    ValueId cursor[] = {rvalue(child(a, args[0]))};
    switch (builtin.op) {
      case ast::VaOp::start:
        stateful(ir::Op::va_start, {}, cursor);
        return std::nullopt;
      case ast::VaOp::arg: {
        ir::Ty result[] = {vty(builtin.info.type)};
        return b_.result(stateful(ir::Op::va_arg, result, cursor), 1);
      }
      case ast::VaOp::copy: {
        ValueId operands[] = {cursor[0], rvalue(child(a, args[1]))};
        stateful(ir::Op::va_copy, {}, operands);
        return std::nullopt;
      }
      case ast::VaOp::end:
        stateful(ir::Op::va_end, {}, cursor);
        return std::nullopt;
    }
    std::unreachable();
  }

  // A scalar default's receiver is represented only by its initialized field values.
  // Recognize side-effect-free designators of that receiver's own non-reference fields.
  std::optional<uint32_t> default_field(At operand) {
    const auto owner = fn_.initialized_fields.owner;
    if (owner == ast::EntityId::none || this_) return std::nullopt;
    operand = strip_parens(operand);
    if (!is<ast::Member>(operand)) return std::nullopt;
    const auto member = get<ast::Member>(operand);
    if (member.reference) return std::nullopt;
    At receiver = strip_parens(child(operand, member.object));
    if (!member.arrow) {
      if (!is<ast::Unary>(receiver)) return std::nullopt;
      const auto dereference = get<ast::Unary>(receiver);
      if (dereference.op != ast::UnaryOp::deref) return std::nullopt;
      receiver = strip_parens(child(receiver, dereference.operand));
    }
    if (!is<ast::This>(receiver) || facts_.class_entity(facts_.pointee(info(receiver).type)) != owner)
      return std::nullopt;
    return member.field;
  }

  ValueId load_glvalue(At operand, ast::QualType type) {
    if (fn_.initialized_fields.owner != ast::EntityId::none && !this_) {
      operand = strip_parens(operand);
      // The conversion reads only the glvalue selected by the conditional. Join the
      // values from that read, so neither arm needs an address for any initialized field.
      const auto kind = cls(type);
      if (kind == TypeClass::integer || kind == TypeClass::bool_) {
        if (is<ast::Conditional>(operand))
          return *conditional(operand, false, type).value;
        if (is<ast::OrderedBinary>(operand)) {
          const auto binary = get<ast::OrderedBinary>(operand);
          if (binary.op == ast::OrderedOp::comma) {
            discard(child(operand, binary.lhs));
            return load_glvalue(child(operand, binary.rhs), type);
          }
        }
      }
      if (auto field = default_field(operand)) {
        const auto values = fn_.initialized_fields.values;
        if (*field < values.size()) return constant(type, values[*field]);
        // This field's initialization has not completed. Let a read fail only if it executes.
        return load(begin_object(type, ir::Init::indeterminate));
      }
    }
    return load(lvalue(operand));
  }

  std::optional<ValueId> conversion(At a) {
    auto c = get<ast::ImplicitConversion>(a);
    ast::QualType t = c.info.type;
    At from = child(a, c.operand);
    switch (c.conversion) {
      case ast::ConversionKind::lvalue_to_rvalue: {
        if (!is_nullptr(t)) return load_glvalue(from, t);
        Place p = lvalue(from);
        // [conv.lval]: a std::nullptr_t glvalue is evaluated without accessing its object.
        ir::Ty result[] = {vty(t)};
        ValueId operands[] = {state_, p.ptr};
        return b_.result(emit(ir::Op::nullptr_value, result, operands));
      }
      case ast::ConversionKind::function_to_pointer: return lvalue(from).ptr;
      case ast::ConversionKind::integral:
      case ast::ConversionKind::floating:
      case ast::ConversionKind::floating_integral: return arithmetic(info(from).type, t, rvalue(from));
      case ast::ConversionKind::pointer_to_integral: return pure(ir::Op::ptr_to_int, t, {rvalue(from)});
      case ast::ConversionKind::integral_to_pointer: return pure(ir::Op::int_to_ptr, t, {rvalue(from)});
      case ast::ConversionKind::boolean: return pure(ir::Op::tobool, t, {rvalue(from)});
      case ast::ConversionKind::array_to_pointer: {
        Place arr = lvalue(from);
        ast::QualType elem = facts_.element(arr.type);
        std::uint64_t path[] = {1, ir::encode({ir::Step::elem, 0})};
        return pure(ir::Op::ptr_member, t, {arr.ptr}, {word(ty(elem)), path[0], path[1]});
      }
      case ast::ConversionKind::pointer:
      case ast::ConversionKind::qualification: {
        ValueId v = rvalue(from);
        if (same_type(info(from).type, t)) return v;
        return pure(ir::Op::ptr_cast, t, {v}, {word(ty(t))});
      }
      case ast::ConversionKind::derived_to_base: {
        // A pointer to a derived class's object to its direct base's ([conv.ptr]/3): the base
        // subobject's address, a null pointer to a null pointer.
        ValueId v = rvalue(from);
        ast::QualType b = facts_.pointee(t);
        std::uint64_t path[] = {1, ir::encode({ir::Step::base, std::to_underlying(c.function)})};
        ValueId moved = pure(ir::Op::ptr_member, t, {v}, {word(ty(b)), path[0], path[1]});
        ast::QualType ft = info(from).type;
        ValueId null = pure(ir::Op::ptr_cmp, ast::builtin(facts_.interners(), ast::Builtin::bool_), {v, constant(ft, facts_.null_constant(ft))},
                            {std::to_underlying(ir::Pred::eq)});
        return pure(ir::Op::select, t, {null, constant(t, facts_.null_constant(t)), moved});
      }
      case ast::ConversionKind::base_to_derived: return derived_address(a, facts_.pointee(t), /*pointer=*/true);
      case ast::ConversionKind::to_void:
        discard(from);
        return std::nullopt;
      case ast::ConversionKind::null_pointer:
        // [conv.ptr]: a std::nullptr_t operand is evaluated even though the result is known.
        if (is_nullptr(info(from).type)) discard(from);
        return constant(t, facts_.null_constant(t));
      default: unsupported(a, "of this conversion kind");
    }
  }

  // An arithmetic conversion of `v` from `from` to `to` ([conv.integral], [conv.double],
  // [conv.fpprom], [conv.fpint]); bool counts as an integer here, and conversions to bool are
  // `tobool`.
  ValueId arithmetic(ast::QualType from, ast::QualType to, ValueId v) {
    if (same_type(from, to)) return v;
    bool ff = cls(from) == TypeClass::floating, tf = cls(to) == TypeClass::floating;
    if (ff && cls(to) == TypeClass::bool_) return pure(ir::Op::tobool, to, {v});
    return pure(ff && tf ? ir::Op::fconv : ff ? ir::Op::f2i : tf ? ir::Op::i2f : ir::Op::iconv, to, {v});
  }

  // `x + 1` or `x - 1` of `x`'s type, for `++` and `--` ([expr.pre.incr], [expr.post.incr]): the
  // arithmetic is done in the promoted type, so a narrow type wraps through the conversion back.
  ValueId step(ast::QualType t, ValueId old, bool inc, std::uint8_t bit_width) {
    if (cls(t) == TypeClass::pointer) {
      ValueId one = int_constant(facts_.int_type(), inc ? 1 : -1);
      return pure(ir::Op::ptr_add, t, {old, one}, {word(ty(facts_.pointee(t)))});
    }
    if (cls(t) == TypeClass::floating)
      return pure(inc ? ir::Op::fadd : ir::Op::fsub, t, {old, float_constant(t, 1.0)});
    if (cls(t) != TypeClass::integer) throw Unsupported{"++ or -- of a non-integer type"};
    ast::QualType p = facts_.promoted(t, bit_width);
    ValueId v = same_type(p, t) ? old : pure(ir::Op::iconv, p, {old});
    ValueId r = arith(inc ? ast::BinaryOp::add : ast::BinaryOp::sub, p, v, int_constant(p, 1));
    return same_type(p, t) ? r : pure(ir::Op::iconv, t, {r});
  }

  std::optional<ValueId> unary(At a) {
    auto u = get<ast::Unary>(a);
    ast::QualType t = u.info.type;
    At o = child(a, u.operand);
    switch (u.op) {
      case ast::UnaryOp::plus: return rvalue(o);
      case ast::UnaryOp::minus: {
        ValueId v = rvalue(o);
        if (cls(t) == TypeClass::floating) return pure(ir::Op::fneg, t, {v});
        return pure(ir::Op::neg, t, {v}, {}, is_signed(t) ? ir::bit(ir::Flag::nsw) : 0);
      }
      case ast::UnaryOp::not_: return pure(ir::Op::xor_, t, {rvalue(o), int_constant(t, 1)});
      case ast::UnaryOp::compl_: return pure(ir::Op::xor_, t, {rvalue(o), int_constant(t, -1)});
      case ast::UnaryOp::address_of: return address(o);
      case ast::UnaryOp::post_inc:
      case ast::UnaryOp::post_dec: {
        Place p = lvalue(o);
        ValueId old = load(p);
        store(p, step(p.type, old, u.op == ast::UnaryOp::post_inc, info(o).bit_width));
        return old;
      }
      default: unsupported(a, "as a prvalue");
    }
  }

  // An arithmetic or bitwise operation on two values of the computation type `t`.
  ValueId arith(ast::BinaryOp op, ast::QualType t, ValueId x, ValueId y) {
    if (cls(t) == TypeClass::floating) {
      switch (op) {
        case ast::BinaryOp::add: return pure(ir::Op::fadd, t, {x, y});
        case ast::BinaryOp::sub: return pure(ir::Op::fsub, t, {x, y});
        case ast::BinaryOp::mul: return pure(ir::Op::fmul, t, {x, y});
        case ast::BinaryOp::div: return pure(ir::Op::fdiv, t, {x, y});
        default: throw Unsupported{"this floating-point operator"};
      }
    }
    bool s = is_signed(t);
    ir::Flags nsw = s ? ir::bit(ir::Flag::nsw) : 0;
    switch (op) {
      case ast::BinaryOp::add: return pure(ir::Op::add, t, {x, y}, {}, nsw);
      case ast::BinaryOp::sub: return pure(ir::Op::sub, t, {x, y}, {}, nsw);
      case ast::BinaryOp::mul: return pure(ir::Op::mul, t, {x, y}, {}, nsw);
      case ast::BinaryOp::div: return pure(s ? ir::Op::sdiv : ir::Op::udiv, t, {x, y});
      case ast::BinaryOp::rem: return pure(s ? ir::Op::srem : ir::Op::urem, t, {x, y});
      case ast::BinaryOp::bit_and: return pure(ir::Op::and_, t, {x, y});
      case ast::BinaryOp::bit_or: return pure(ir::Op::or_, t, {x, y});
      case ast::BinaryOp::bit_xor: return pure(ir::Op::xor_, t, {x, y});
      default: throw Unsupported{"this arithmetic operator"};
    }
  }
  // `x << y` and `x >> y` in the promoted type of `x` ([expr.shift]); `y` keeps its own type.
  ValueId shift(bool left, ast::QualType t, ValueId x, ValueId y) {
    if (left) return pure(ir::Op::shl, t, {x, y});
    return pure(is_signed(t) ? ir::Op::ashr : ir::Op::lshr, t, {x, y});
  }

  // Pointer arithmetic ([expr.add]): pointer ± integer, integer + pointer, pointer - pointer.
  ValueId pointer_arith(ast::BinaryOp op, ast::QualType t, At l, ValueId x, At r, ValueId y) {
    bool lp = cls(info(l).type) == TypeClass::pointer, rp = cls(info(r).type) == TypeClass::pointer;
    if (lp && rp) return pure(ir::Op::ptr_diff, t, {x, y}, {word(ty(facts_.pointee(info(l).type)))});
    ValueId p = lp ? x : y, i = lp ? y : x;
    ast::QualType it = lp ? info(r).type : info(l).type;
    if (op == ast::BinaryOp::sub) i = negated_index(it, i);
    return pure(ir::Op::ptr_add, t, {p, i}, {word(ty(facts_.pointee(t)))});
  }
  // `-j` for `P - j` ([expr.add]), in `ptrdiff_t`: negating in `j`'s own type would turn an
  // unsigned `j` into a huge positive index. No `nsw`: [expr.add] defines `P - j` whenever the
  // result is in the array, and only a `j` that no array can satisfy overflows.
  ValueId negated_index(ast::QualType it, ValueId i) {
    ast::QualType d = facts_.ptrdiff_type();
    if (!same_type(it, d)) i = pure(ir::Op::iconv, d, {i});
    return pure(ir::Op::neg, d, {i});
  }

  std::optional<ValueId> binary(At a) {
    auto bn = get<ast::Binary>(a);
    At l = child(a, bn.lhs), r = child(a, bn.rhs);
    ValueId x{}, y{};
    unsequenced(std::array{l, r}, [&](std::size_t i, At o) { (i ? y : x) = rvalue(o); });
    ast::QualType t = bn.info.type, ot = info(l).type;
    using enum ast::BinaryOp;
    // [expr.eq]: two std::nullptr_t values compare equal after both operands are evaluated.
    if ((bn.op == eq || bn.op == ne) && is_nullptr(ot) && is_nullptr(info(r).type))
      return int_constant(t, bn.op == eq);
    switch (bn.op) {
      case lt: case gt: case le: case ge: case eq: case ne: {
        if (cls(ot) == TypeClass::pointer) {
          ir::Pred p = bn.op == lt ? ir::Pred::lt : bn.op == gt ? ir::Pred::gt : bn.op == le ? ir::Pred::le
                     : bn.op == ge ? ir::Pred::ge : bn.op == eq ? ir::Pred::eq : ir::Pred::ne;
          return pure(ir::Op::ptr_cmp, t, {x, y}, {std::to_underlying(p)});
        }
        if (cls(ot) == TypeClass::floating) {
          // [expr.rel], [expr.eq]: ordered, so false when either is a NaN; `!=` is true then.
          ir::Pred p = bn.op == lt ? ir::Pred::lt : bn.op == gt ? ir::Pred::gt : bn.op == le ? ir::Pred::le
                     : bn.op == ge ? ir::Pred::ge : bn.op == eq ? ir::Pred::eq : ir::Pred::ne;
          return pure(ir::Op::fcmp, t, {x, y}, {std::to_underlying(p)});
        }
        bool s = is_signed(ot);
        ir::Pred p = bn.op == eq ? ir::Pred::eq : bn.op == ne ? ir::Pred::ne
                   : bn.op == lt ? (s ? ir::Pred::slt : ir::Pred::ult)
                   : bn.op == gt ? (s ? ir::Pred::sgt : ir::Pred::ugt)
                   : bn.op == le ? (s ? ir::Pred::sle : ir::Pred::ule)
                                 : (s ? ir::Pred::sge : ir::Pred::uge);
        return pure(ir::Op::icmp, t, {x, y}, {std::to_underlying(p)});
      }
      case add: case sub:
        if (cls(t) == TypeClass::pointer || cls(ot) == TypeClass::pointer) return pointer_arith(bn.op, t, l, x, r, y);
        return arith(bn.op, t, x, y);
      default: return arith(bn.op, t, x, y);
    }
  }

  std::optional<ValueId> ordered(At a) {
    auto o = get<ast::OrderedBinary>(a);
    At l = child(a, o.lhs), r = child(a, o.rhs);
    ast::QualType t = o.info.type;
    switch (o.op) {
      case ast::OrderedOp::shl:
      case ast::OrderedOp::shr: {
        ValueId x = rvalue(l);
        ValueId y = rvalue(r);
        return shift(o.op == ast::OrderedOp::shl, t, x, y);
      }
      case ast::OrderedOp::logical_and:
      case ast::OrderedOp::logical_or: {
        // [expr.log.and], [expr.log.or]: the right operand is evaluated only if the left one does
        // not decide; the join receives whichever decided.
        bool is_and = o.op == ast::OrderedOp::logical_and;
        ValueId x = rvalue(l);
        const auto rhs = new_block();
        ArmExit left{block_, state_, x};
        ++branches_;
        ArmExit arms[] = {std::move(left), evaluate_arm(rhs, [&] { return rvalue(r); })};
        --branches_;
        return join_arms(arms, t, ShortCircuit{rhs, is_and});
      }
      case ast::OrderedOp::comma:
        discard(l);
        return rvalue_or_void(r);
      default: unsupported(a, "pointer-to-member access");
    }
  }

  // The scalar initialization may run after storage is acquired only when it cannot throw.
  // Calls and temporaries need an exception analysis or an EH cleanup edge ([expr.new]).
  bool nonthrowing_initializer(At a) {
    if (is<ast::ConstantProbe>(a)) return true;
    if (is<ast::ImplicitConversion>(a) &&
        get<ast::ImplicitConversion>(a).conversion == ast::ConversionKind::user_defined) return false;
    if (!(is<ast::Literal>(a) || is<ast::DeclRef>(a) || is<ast::LocalRef>(a) || is<ast::This>(a) ||
          is<ast::ValueInit>(a) || is<ast::DefaultInit>(a) || is<ast::Paren>(a) || is<ast::Unary>(a) ||
          is<ast::Binary>(a) || is<ast::OrderedBinary>(a) || is<ast::Assign>(a) || is<ast::Conditional>(a) ||
          is<ast::Subscript>(a) || is<ast::Member>(a) || is<ast::ImplicitConversion>(a) ||
          is<ast::BuiltinCall>(a) || is<ast::AtomicCall>(a) || is<ast::VariadicBuiltin>(a) ||
          is<ast::ThreeWay>(a) || is<ast::BitCast>(a) ||
          is<ast::SourceLine>(a) || is<ast::SourceLocation>(a) || is<ast::DefaultSourceContext>(a)))
      return false;
    bool proven = true;
    ast::Red(ast_, a.id).for_each_child([&](const ast::Red& c) {
      proven = proven && nonthrowing_initializer({c.id(), 0});
    });
    return proven;
  }

  ValueId new_object(At a) {
    auto n = get<ast::NewObject>(a);
    At init = child(a, n.init);
    ast::QualType p = n.info.type;
    // A class's constructor may throw, and [expr.new]/28 then calls the deallocation function.
    // That needs an exceptional edge, which only certified frames get (#1135). A frame with a
    // new-expression is never certified (the pipeline's unwind proof admits no NewObject), so
    // the frame has no unwind information and such an exception terminates the program there,
    // before a caller could observe the unreleased storage.
    if (!fn_.thunk && cls(facts_.pointee(p)) != TypeClass::record && !nonthrowing_initializer(init))
      throw Unsupported{"cleanup for a potentially throwing new-initializer"};
    ValueId storage = rvalue(child(a, n.allocation));
    if (n.placement) {
      ValueId objects[] = {storage};
      std::uint64_t types[] = {word(ty(facts_.pointee(p)))};
      stateful(ir::Op::obj__reuse, {}, objects, types);
    }
    ValueId object = pure(ir::Op::ptr_cast, p, {storage}, {word(ty(p))});
    // Evaluation stops at an ordinary allocator or at storage reuse. A namespace-scope
    // initializer can then fall back to runtime without evaluating the new-initializer.
    if (fn_.thunk) return object;
    ast::QualType t = facts_.pointee(p);
    // [dcl.init.general]/9: value-initialization zero-initializes a class object before any
    // constructor runs, and the allocated storage is not zero. Copy a zero object into it.
    // Zero it in place: a zero temporary would put the whole object on the stack.
    const bool zeroed = cls(t) == TypeClass::record && value_initializes(init);
    if (zeroed) zero_bytes(storage, allocated_size(child(a, n.allocation)));
    initialize({object, t}, init, zeroed);
    ValueId objects[] = {object};
    std::uint64_t types[] = {word(ty(t))};
    stateful(ir::Op::obj__begin, {}, objects, types);
    return object;
  }

  // The byte count a new-expression passes to its allocation function: sema's `sizeof(T)`.
  std::uint64_t allocated_size(At allocation) {
    if (!is<ast::Call>(allocation)) unsupported(allocation, "as an allocation");
    const auto args = ast_.children(get<ast::Call>(allocation).args);
    At size = args.empty() ? At{} : strip_parens(child(allocation, args.front()));
    if (args.empty() || !is<ast::Literal>(size) || !in_.values.is<ast::VInt>(get<ast::Literal>(size).value))
      unsupported(allocation, "with a computed size");
    return in_.values.get<ast::VInt>(get<ast::Literal>(size).value).bits.lo;
  }
  // Stores zero to each of `count` bytes from `storage`, in a loop.
  void zero_bytes(ValueId storage, std::uint64_t count) {
    const auto byte = ast::builtin(facts_.interners(), ast::Builtin::unsigned_char);
    const auto bytes = facts_.pointer_to(byte);
    const ValueId first = pure(ir::Op::ptr_cast, bytes, {storage}, {word(ty(bytes))});
    const ast::QualType index_type = facts_.ptrdiff_type();
    const ValueId end = int_constant(index_type, std::int64_t(count));
    ir::BlockId head = new_block({index_type}), body = new_block(), done = new_block();
    br(head, {int_constant(index_type, 0)});
    enter(head);
    const ValueId index = b_.param(head, 1);
    cond_br(pure(ir::Op::icmp, facts_.bool_type(), {index, end}, {std::to_underlying(ir::Pred::slt)}), edge(body), edge(done));
    enter(body);
    store({pure(ir::Op::ptr_add, bytes, {first, index}, {word(ty(byte))}), byte}, zero(byte));
    br(head, {arith(ast::BinaryOp::add, index_type, index, int_constant(index_type, 1))});
    enter(done);
  }

  ValueId new_array(At a) {
    const auto n = get<ast::NewArray>(a);
    if (!fn_.native_array_new || n.allocation.id == NodeId::none) throw Unsupported{"array allocation"};
    // Scalar default-initialization leaves the bytes alone. The allocator returned the array address.
    ValueId storage = rvalue(child(a, n.allocation));
    return pure(ir::Op::ptr_cast, n.info.type, {storage}, {word(ty(n.info.type))});
  }

  void if_nonnull(ValueId addr, auto&& body) {
    ValueId nonnull = pure(ir::Op::tobool, facts_.bool_type(), {addr});
    ir::BlockId release = new_block(), join = new_block();
    cond_br(nonnull, edge(release), edge(join));
    enter(release);
    body();
    br(join);
    enter(join);
  }

  void delete_object(At a) {
    if (fn_.thunk) unsupported(a, "in constant evaluation");
    auto d = get<ast::DeleteObject>(a);
    At operand = child(a, d.address);
    ValueId object = rvalue(operand);
    if_nonnull(object, [&] {
      // [expr.delete]/6: the destructor ends the object, then its storage is released.
      if (d.destructor != ast::EntityId::none) {
        ValueId self[] = {object};
        call_fn(d.destructor, facts_.int_type(), self, ImplicitObject::constructor_or_destructor, true);
      }
      stateful(ir::Op::obj__end, {}, std::span(&object, 1));
      ast::QualType void_pointer = facts_.pointer_to(d.info.type);
      ValueId address = pure(ir::Op::ptr_cast, void_pointer, {object}, {word(ty(void_pointer))});
      ValueId args[] = {address, rvalue(child(a, d.size))};
      call_fn(d.function, d.info.type, args);
    });
  }

  void delete_array(At a) {
    if (!fn_.native_array_delete) throw Unsupported{"array deallocation"};
    auto d = get<ast::DeleteArray>(a);
    ValueId operand = rvalue(child(a, d.operand));
    if_nonnull(operand, [&] {
      ast::QualType void_pointer = facts_.pointer_to(d.info.type);
      ValueId address = pure(ir::Op::ptr_cast, void_pointer, {operand}, {word(ty(void_pointer))});
      // The call releases the allocation root, including a zero-length array ([basic.life]).
      call_fn(d.function, d.info.type, std::span(&address, 1));
    });
  }

  // A call to a named function ([expr.call]). The callee is not evaluated: naming a function has
  // no effect. Arguments are evaluated in order, which is one of the indeterminately sequenced
  // orders the clause allows.
  std::optional<ValueId> call(At a, std::optional<ValueId> into = std::nullopt) {
    auto c = get<ast::Call>(a);
    if (a.id == fn_.cleanup_call) {
      const auto normal = b_.add_block({});
      const ir::Ty pending[] = {ir::Ty::state(), ir::Ty::exn()};
      const auto exceptional = b_.add_block(pending);
      const ValueId args[] = {state_};
      const ir::Ty results[] = {ir::Ty::state()};
      const std::uint64_t imms[] = {word(c.function), 0};
      const ir::SuccRef successors[] = {{normal, {}}, {exceptional, {}}};
      const auto invoke = emit(ir::Op::invoke, results, args, imms, ir::bit(ir::Flag::not_constexpr), successors);
      enter(exceptional);
      exit_scopes(0);
      if (fn_.constructor_cleanup != ast::EntityId::none) {
        if (!initialized_member_) throw Unsupported{"unwind before the union member's initialization"};
        destroy(*initialized_member_, fn_.constructor_cleanup);
      }
      const ValueId resume[] = {state_, b_.param(exceptional, 1)};
      emit(ir::Op::resume, {}, resume);
      block_ = normal;
      state_ = b_.result(invoke, 0);
      return std::nullopt;
    }
    const auto checks = fn_.unwind_cleanup_checks;
    const bool check_unwind = checks == UnwindCleanupChecks::all_calls ||
                              (checks == UnwindCleanupChecks::aggregate_defaults && in_aggregate_default_);
    if (check_unwind && std::ranges::any_of(scopes_, [](const auto& scope) {
          return std::ranges::any_of(scope, [](const Ending& e) { return e.destructor != ast::EntityId::none; });
        })) {
      ast::QualType callee = info(child(a, c.callee)).type;
      if (c.function == ast::EntityId::none) callee = facts_.pointee(callee);
      const auto function = in_.types.canon(callee);
      if (in_.types.is<ast::TFunction>(function)) {
        const auto spec = in_.types.get<ast::TFunction>(function).noexcept_;
        if (spec == ast::ConstId::none || !in_.values.is<ast::VInt>(spec) || in_.values.get<ast::VInt>(spec).bits.lo == 0)
          throw Unsupported{"a potentially throwing call with an unsupported local unwind cleanup"};
      }
    }
    if (class_prvalue(c.info) && !into) unsupported(a, "of class type outside an initialization");
    std::vector<ValueId> args;
    // A call through a pointer to function ([expr.call]/1): the callee, a prvalue pointer, is
    // evaluated first, then the arguments ([expr.call]/7).
    std::optional<ast::QualType> through;
    if (c.function == ast::EntityId::none) {
      At callee = child(a, c.callee);
      through = facts_.pointee(info(callee).type);
      args.push_back(rvalue(callee));
    }
    if (into) args.push_back(*into);
    const std::size_t callee_slots = through ? 1 : 0;
    uint32_t byval = 0;
    for (ast::Child arg : ast_.children(c.args)) {
      At x = child(a, arg);
      // An argument object of class type: sema materialized it ([expr.call]/7).
      if (argument_object(x)) {
        const std::size_t index = args.size() - callee_slots;
        if (index >= 32) throw Unsupported{"a call with more than 32 arguments"};
        byval |= 1u << index;
      }
      args.push_back(argument(x));
    }
    if (through) return call_pointer(*through, result_type(c.info), args, byval, into ? 1 : 0);
    if (c.builtin != ast::LibraryBuiltin::none) return library_builtin(c, args);
    return call_fn(c.function, result_type(c.info), args, ImplicitObject::none, false, byval, into ? 1 : 0);
  }
  // A builtin's call of a C library function, its arguments evaluated: in constant evaluation the
  // interpreter reads the bytes (`ir::BuiltinFn`); at run time it is the library call, and `lower`
  // keeps only that edge.
  ValueId library_builtin(const ast::Call& c, std::span<const ValueId> args) {
    const ast::QualType result = result_type(c.info);
    const ir::BlockId evaluated = new_block(), library = new_block(), join = new_block({result});
    const ir::SuccRef succs[] = {edge(evaluated), edge(library)};
    emit(ir::Op::consteval_br, {}, {}, {}, 0, succs);
    enter(evaluated);
    const ir::Ty value[] = {vty(result)};
    const auto fn = c.builtin == ast::LibraryBuiltin::memchr ? ir::BuiltinFn::memchr : ir::BuiltinFn::memcmp;
    const std::uint64_t imms[] = {word(fn)};
    const ValueId computed = b_.result(stateful(ir::Op::builtin, value, args, imms), 1);
    br(join, {computed});
    enter(library);
    const ValueId called = *call_fn(c.function, result, args);
    br(join, {called});
    enter(join);
    return b_.param(join, 1);
  }
  // A call through a function pointer, `args[0]`, of function type `fn`: `call_fn`'s `call_ind`.
  // `byval` and `result_arg` already exclude the callee pointer from their indices.
  std::optional<ValueId> call_pointer(ast::QualType fn, ast::QualType result, std::span<const ValueId> args, uint32_t byval,
                                      uint32_t result_arg) {
    std::vector<ir::Ty> results;
    if (args.size() > 33) throw Unsupported{"a call with more than 32 arguments"};
    bool has_value = !result_arg && cls(result) != TypeClass::void_;
    if (has_value) results.push_back(vty(result));
    auto f = in_.types.get<ast::TFunction>(in_.types.canon(fn));
    if (selecting_initializer_ && fn_.exceptions_enabled) {
      const auto spec = f.noexcept_;
      if (spec == ast::ConstId::none || !in_.values.is<ast::VInt>(spec) || in_.values.get<ast::VInt>(spec).bits.lo == 0)
        throw Unsupported{"a potentially throwing indirect call in a class initializer selection"};
    }
    std::size_t named = in_.seqs.size(f.params), passed = args.size() - 1 - (result_arg ? 1 : 0);
    std::size_t through_ellipsis = f.variadic && passed > named ? passed - named : 0;
    std::uint64_t imms[] = {through_ellipsis, byval, result_arg};
    ir::Flags flags = f.variadic ? ir::bit(ir::Flag::variadic) : 0;
    ir::NodeId n = stateful(ir::Op::call_ind, results, args, imms, flags);
    if (has_value) return b_.result(n, 1);
    return std::nullopt;
  }
  // Itanium ABI §3.2.4: the address point sits at offset zero in this root subobject.
  std::optional<ValueId> virtual_member_call(At a) {
    const auto c = get<ast::VirtualMemberCall>(a);
    const auto selection = in_.seqs.at(c.selection, 0);
    if constexpr (F::evaluating_constants) {
      if (selection.function != ast::EntityId::none && c.info.category == ast::ValueCategory::prvalue &&
          !class_prvalue(c.info) && !facts_.ellipsis_after(selection.function))
        return evaluated_virtual_call(a, c.info, selection.function, c.object, c.args, ir::VirtualCallKind::ordinary);
    }
    if (fn_.thunk) unsupported(a, "in constant evaluation");
    const ValueId receiver = rvalue(child(a, c.object));
    const auto table_type = facts_.pointer_to(c.callee);
    const auto receiver_type = facts_.pointer_to(table_type);
    const ValueId vptr = pure(ir::Op::ptr_cast, receiver_type, {receiver}, {word(ty(receiver_type))});
    const ValueId table = load({vptr, table_type});
    const ValueId index = int_constant(facts_.ptrdiff_type(), selection.slot);
    const ValueId slot = pure(ir::Op::ptr_add, table_type, {table, index}, {word(ty(c.callee))});
    std::vector<ValueId> args{load({slot, c.callee}), receiver};
    // Records by value need the by-value bits and result object of `member_call`; sema admits
    // only scalar virtual signatures so far (#309), and the back end no longer rejects records
    // on indirect calls (#640).
    if (class_prvalue(c.info)) unsupported(a, "of a virtual function returning a class");
    for (ast::Child arg : ast_.children(c.args)) {
      At x = child(a, arg);
      if (argument_object(x)) unsupported(a, "of a virtual function taking a class by value");
      args.push_back(argument(x));
    }
    return call_pointer(facts_.pointee(c.callee), result_type(c.info), args, 0, 0);
  }
  std::optional<ValueId> evaluated_virtual_call(At a, ast::ExprInfo info, ast::EntityId function,
                                                ast::Child object, ast::ChildList arguments, ir::VirtualCallKind kind) {
    Position here(*this, a.offset);
    std::vector<ValueId> args{rvalue(child(a, object))};
    for (ast::Child arg : ast_.children(arguments)) {
      At x = child(a, arg);
      if (argument_object(x)) unsupported(a, "of a virtual function with a by-value class argument");
      args.push_back(argument(x));
    }
    if (args.size() > 32) unsupported(a, "with more than 32 arguments");
    const auto result = result_type(info);
    const ir::Ty value[] = {vty(result)};
    const bool has_value = cls(result) != TypeClass::void_;
    const std::uint64_t imms[] = {word(function), word(kind)};
    const auto n = stateful(ir::Op::call_virtual, std::span(value).first(has_value ? 1 : 0), args, imms);
    return has_value ? std::optional(b_.result(n, 1)) : std::nullopt;
  }
  std::optional<ValueId> immediate_virtual_call(At a) {
    const auto call = get<ast::ImmediateVirtualCall>(a);
    const auto kind = call.qualified ? ir::VirtualCallKind::qualified_immediate : ir::VirtualCallKind::immediate;
    return evaluated_virtual_call(a, call.info, call.function, call.object, call.args, kind);
  }

  bool member_call_expression(At a) {
    return is<ast::MemberCall>(a) || is<ast::VirtualMemberCall>(a) || is<ast::ImmediateVirtualCall>(a) ||
           is<ast::AssignmentCall>(a);
  }
  // A non-static member function's call: the object's address is the first ABI argument.
  std::optional<ValueId> member_call(At a, std::optional<ValueId> into = std::nullopt) {
    if (is<ast::VirtualMemberCall>(a)) return virtual_member_call(a);
    if (is<ast::ImmediateVirtualCall>(a)) return immediate_virtual_call(a);
    if (is<ast::AssignmentCall>(a)) {
      auto c = get<ast::AssignmentCall>(a);
      if (class_prvalue(c.info) && !into) unsupported(a, "of class type outside an initialization");
      At rhs = child(a, c.rhs);
      ValueId value = argument(rhs);
      std::vector<ValueId> args{rvalue(child(a, c.object))};
      if (into) args.push_back(*into);
      const uint32_t byval = argument_object(rhs) ? 1u << args.size() : 0;
      args.push_back(value);
      return call_fn(c.function, result_type(c.info), args, ImplicitObject::member, false, byval, into ? 2 : 0);
    }
    auto c = get<ast::MemberCall>(a);
    if (class_prvalue(c.info) && !into) unsupported(a, "of class type outside an initialization");
    std::vector<ValueId> args{rvalue(child(a, c.object))};
    if (into) args.push_back(*into);
    uint32_t byval = 0;
    for (ast::Child arg : ast_.children(c.args)) {
      At x = child(a, arg);
      if (argument_object(x)) byval |= 1u << args.size();
      args.push_back(argument(x));
    }
    return call_fn(c.function, result_type(c.info), args, ImplicitObject::member, false, byval, into ? 2 : 0);
  }
  // A call whose result is a class object of its own, which a hidden argument names
  // ([stmt.return]); a call returning a reference to a class is a glvalue.
  bool class_prvalue(const ast::ExprInfo& i) {
    return i.category == ast::ValueCategory::prvalue && cls(i.type) == TypeClass::record;
  }
  // An argument object for a parameter of class type, which sema marks ([expr.call]/7).
  bool argument_object(At x) {
    x = strip_parens(x);
    return is<ast::Materialize>(x) && get<ast::Materialize>(x).extended == 2;
  }
  At strip_parens(At a) {
    while (is<ast::Paren>(a)) a = child(a, get<ast::Paren>(a).operand);
    return a;
  }
  // The IR type of a call's result: a glvalue's is a pointer to its object ([expr.call]).
  ast::QualType result_type(const ast::ExprInfo& i) {
    return i.category == ast::ValueCategory::prvalue ? i.type : facts_.pointer_to(i.type);
  }
  enum class ImplicitObject { none, member, constructor_or_destructor };

  // A call to `f`. An implicit object argument precedes the parameters. The arguments following
  // the parameters match the ellipsis ([expr.call]/12), and sema has already promoted them.
  // `byval` and `result_arg` are the call op's masks (ir/ops.cppm); a class result is written
  // through the argument `result_arg` names and gives no value.
  std::optional<ValueId> call_fn(ast::EntityId f, ast::QualType result, std::span<const ValueId> args,
                                 ImplicitObject object = ImplicitObject::none, bool void_ = false,
                                 uint32_t byval = 0, uint32_t result_arg = 0,
                                 ir::Flags flags = 0) {
    // The selected arm has a normal cleanup edge. An exception would also need to destroy
    // any completed argument, older condition temporaries and, during cleanup, the result.
    if (selecting_initializer_ && fn_.exceptions_enabled && !std::ranges::binary_search(fn_.nonthrowing_functions, f))
      throw Unsupported{"a potentially throwing call in a class initializer selection"};
    std::vector<ir::Ty> results;
    if (args.size() > 32) throw Unsupported{"a call with more than 32 arguments"};
    bool has_value = !void_ && !result_arg && cls(result) != TypeClass::void_;
    if (has_value) results.push_back(vty(result));
    std::size_t through_ellipsis = 0;
    std::size_t hidden = (object != ImplicitObject::none ? 1 : 0) + (result_arg ? 1 : 0);
    const auto named = facts_.ellipsis_after(f);
    if (named && args.size() > hidden + *named)
      through_ellipsis = args.size() - hidden - *named;
    std::uint64_t imms[] = {word(f), through_ellipsis, byval, result_arg};
    if (!facts_.is_constexpr(f)) flags |= ir::bit(ir::Flag::not_constexpr);
    // Constant evaluation rejects a non-constexpr call itself ([expr.const]). Check the
    // receiver only when the interpreter could enter an ordinary member's body.
    if (object == ImplicitObject::member && !(flags & ir::bit(ir::Flag::not_constexpr)))
      stateful(ir::Op::obj__check_receiver, {}, args.first(1));
    if (named) flags |= ir::bit(ir::Flag::variadic);
    ir::NodeId n = stateful(ir::Op::call, results, args, imms, flags);
    if (has_value) return b_.result(n, 1);
    return std::nullopt;
  }

  // `c ? x : y` ([expr.cond]): a join with the chosen value, or for a glvalue the chosen object.
  struct Chosen {
    std::optional<ValueId> value;
    Place place;
  };
  Chosen conditional(At a, bool glvalue, std::optional<ast::QualType> load_type = std::nullopt) {
    auto c = get<ast::Conditional>(a);
    ast::QualType t = load_type.value_or(c.info.type);
    bool has_value = glvalue || cls(t) != TypeClass::void_;
    ast::QualType jt = glvalue ? facts_.pointer_to(t) : t;
    ValueId cv = rvalue(child(a, c.condition));
    ir::BlockId then_b = new_block(), else_b = new_block();
    const bool bit_field_result = glvalue && c.info.bit_width != 0;
    Place bit_field{};
    cond_br(cv, edge(then_b), edge(else_b));
    auto operand = [&](At x) -> std::optional<ValueId> {
      if (!has_value) {
        discard(x);
        return {};
      }
      if (cls(info(x).type) == TypeClass::void_) {
        // This arm is a throw-expression and never supplies a value to the join.
        discard(x);
        emit(ir::Op::unreachable, {}, {});
        live_ = false;
        return {};
      }
      if (!glvalue) return load_type ? load_glvalue(x, t) : rvalue(x);
      Place place = lvalue(x);
      if (!bit_field_result) return place.ptr;
      // The only arm that reaches the join owns the enclosing record pointer.
      bit_field = place;
      return {};
    };
    ++branches_;
    ArmExit arms[] = {evaluate_arm(then_b, [&] { return operand(child(a, c.then_)); }),
                      evaluate_arm(else_b, [&] { return operand(child(a, c.else_)); })};
    --branches_;
    auto value = join_arms(arms, has_value && !bit_field_result ? std::optional(jt) : std::nullopt);
    if (!has_value) return {};
    if (bit_field_result) return {std::nullopt, bit_field};
    ValueId v = *value;
    return {glvalue ? std::nullopt : std::optional(v), {v, t}};
  }

  // The object a glvalue designates ([basic.lval]).
  Place lvalue(At a) {
    Position here(*this, a.offset);
    ast::ExprInfo ei = info(a);
    if (ei.category == ast::ValueCategory::prvalue) unsupported(a, "as a prvalue where a glvalue is expected");
    ast::QualType t = ei.type;
    if (is<ast::LocalRef>(a)) {
      uint32_t i = get<ast::LocalRef>(a).local;
      if (i >= slots_.size()) unsupported(a, "naming a local that is not declared yet");
      if (!slots_[i]) {
        const ast::LocalRec rec = in_.seqs.at(fn_.body.locals, i);
        if (!fn_.thunk && rec.entity != ast::EntityId::none && !facts_.is_reference(rec.type))
          return static_place(rec);
        unsupported(a, "naming a local that is not declared yet");
      }
      Place p = *slots_[i];
      if (auto binding = jump_bindings_.find(i); binding != jump_bindings_.end()) p.ptr = load(binding->second);
      return p;
    }
    // A derived class's object as its direct base's ([conv.ptr]/3, [dcl.init.ref]): the base
    // subobject, named by the base class's entity.
    if (is<ast::ImplicitConversion>(a)) {
      const auto c = get<ast::ImplicitConversion>(a);
      if (c.conversion == ast::ConversionKind::derived_to_base)
        return subobject(lvalue(child(a, c.operand)), ir::Step::base, std::to_underlying(c.function), t);
      if (c.conversion == ast::ConversionKind::base_to_derived)
        return {derived_address(a, t, /*pointer=*/false), t};
      if (c.conversion == ast::ConversionKind::qualification) {
        // An explicit xvalue cast preserves storage while carrying the target type's qualifiers.
        Place p = lvalue(child(a, c.operand));
        if (!same_type(p.type, t)) {
          const auto ptr_type = facts_.pointer_to(t);
          p.ptr = pure(ir::Op::ptr_cast, ptr_type, {p.ptr}, {word(ty(ptr_type))});
        }
        p.type = t;
        return p;
      }
    }
    if (is<ast::DeclRef>(a)) {
      auto d = get<ast::DeclRef>(a);
      ast::QualType ptr = facts_.pointer_to(t);
      // A namespace-scope reference's symbol holds its referent's address ([dcl.ref]).
      if (d.reference) return {load({pure(ir::Op::global, facts_.pointer_to(ptr), {}, {word(d.entity)}), ptr}), t};
      // A function designator's address is the function's ([conv.func], [expr.unary.op]).
      if (cls(t) == TypeClass::function) return {pure(ir::Op::func_addr, ptr, {}, {word(d.entity)}), t};
      // A namespace-scope variable: its address is a constant of the entity ([basic.stc.static]).
      return {pure(ir::Op::global, ptr, {}, {word(d.entity)}), t};
    }
    if (is<ast::Literal>(a)) {
      // A string literal names a static array object ([lex.string]).
      ast::ConstId addr = facts_.string_address(get<ast::Literal>(a).value);
      return {constant(facts_.pointer_to(t), addr), t};
    }
    if (is<ast::Paren>(a)) return lvalue(child(a, get<ast::Paren>(a).operand));
    if (is<ast::DefaultArg>(a)) return pinned([&] { return lvalue(child(a, get<ast::DefaultArg>(a).arg)); });
    if (is<ast::DefaultSourceContext>(a)) return with_default_source(a, [&](At arg) { return lvalue(arg); });
    if (is<ast::Unary>(a)) {
      auto u = get<ast::Unary>(a);
      At o = child(a, u.operand);
      if (u.op == ast::UnaryOp::deref) return {rvalue(o), t};
      if (u.op == ast::UnaryOp::pre_inc || u.op == ast::UnaryOp::pre_dec) {
        Place p = lvalue(o);
        store(p, step(p.type, load(p), u.op == ast::UnaryOp::pre_inc, info(o).bit_width));
        return p;
      }
      unsupported(a, "as a glvalue");
    }
    if (is<ast::Subscript>(a)) {
      // [expr.sub]: `E1[E2]` is `*(E1 + E2)`, with E1 sequenced before E2.
      auto s = get<ast::Subscript>(a);
      At base = child(a, s.base), index = child(a, s.index);
      ValueId x = rvalue(base);
      ValueId y = rvalue(index);
      bool base_is_ptr = cls(info(base).type) == TypeClass::pointer;
      ValueId p = base_is_ptr ? x : y, i = base_is_ptr ? y : x;
      return {pure(ir::Op::ptr_add, facts_.pointer_to(t), {p, i}, {word(ty(t))}), t};
    }
    if (is<ast::Member>(a)) {
      // [expr.ref]: the member subobject of the object, or of the object the pointer points to;
      // for a reference member, the object it refers to.
      auto m = get<ast::Member>(a);
      At o = child(a, m.object);
      ValueId base = m.arrow ? rvalue(o) : lvalue(o).ptr;
      auto owner = m.arrow ? facts_.pointee(info(o).type) : info(o).type;
      if (!m.reference) return subobject({base, owner}, ir::Step::field, m.field, t);
      Place ref = subobject({base, owner}, ir::Step::field, m.field, facts_.pointer_to(t));
      return {load(ref), t};
    }
    if (is<ast::Materialize>(a)) return materialize(a);
    if (is<ast::Call>(a) || member_call_expression(a)) {
      auto v = is<ast::Call>(a) ? call(a) : member_call(a);
      return {*v, t};  // a call returning a reference designates the object it refers to
    }
    if (is<ast::Assign>(a)) return assign(a);
    if (is<ast::Conditional>(a)) return conditional(a, true).place;
    if (is<ast::OrderedBinary>(a)) {
      auto o = get<ast::OrderedBinary>(a);
      if (o.op == ast::OrderedOp::comma) {
        discard(child(a, o.lhs));
        return lvalue(child(a, o.rhs));
      }
      if (o.op == ast::OrderedOp::dot_star || o.op == ast::OrderedOp::arrow_star)
        unsupported(a, "with a pointer-to-member value");
    }
    unsupported(a, "as a glvalue");
  }

  // `a = b` and `a op= b` ([expr.assign]): the right operand is sequenced before the left. In a
  // compound assignment the right operand already has the computation type (for shifts, its own
  // promoted type); the left is loaded once, converted to it, and the result converted back.
  Place assign(At a) {
    auto s = get<ast::Assign>(a);
    // An array is assigned as a whole only in a defaulted or implicit assignment operator's definition.
    if (cls(s.info.type) == TypeClass::record || s.op == ast::AssignOp::trivial_class_copy) return assign_class(a);
    ValueId v = rvalue(child(a, s.rhs));
    At rhs = child(a, s.rhs);
    Place p = lvalue(child(a, s.lhs));
    if (s.op == ast::AssignOp::assign) {
      store(p, v);
      return p;
    }
    ValueId old = load(p);
    ValueId r{};
    if (cls(p.type) == TypeClass::pointer) {
      if (s.op != ast::AssignOp::add && s.op != ast::AssignOp::sub) unsupported(a, "on a pointer");
      ast::QualType it = info(rhs).type;  // an integer; `computation` is the pointer type
      ValueId i = s.op == ast::AssignOp::add ? v : negated_index(it, v);
      r = pure(ir::Op::ptr_add, p.type, {old, i}, {word(ty(facts_.pointee(p.type)))});
    } else {
      bool is_shift = s.op == ast::AssignOp::shl || s.op == ast::AssignOp::shr;
      // The left operand is converted to the computation type and the result back ([expr.assign]).
      ast::QualType ct = s.computation;
      ValueId x = arithmetic(p.type, ct, old);
      if (is_shift) {
        r = shift(s.op == ast::AssignOp::shl, ct, x, v);
      } else {
        using enum ast::AssignOp;
        ast::BinaryOp op = s.op == mul ? ast::BinaryOp::mul : s.op == div ? ast::BinaryOp::div
                         : s.op == rem ? ast::BinaryOp::rem : s.op == add ? ast::BinaryOp::add
                         : s.op == sub ? ast::BinaryOp::sub : s.op == bit_and ? ast::BinaryOp::bit_and
                         : s.op == bit_xor ? ast::BinaryOp::bit_xor : ast::BinaryOp::bit_or;
        r = arith(op, ct, x, v);
      }
      r = arithmetic(ct, p.type, r);
    }
    store(p, r);
    return p;
  }

  // Assignment of a class object by its trivial copy assignment operator ([class.copy.assign]):
  // from a glvalue, a copy of its object; from a braced list or a call's result, a copy of the
  // temporary it initializes ([expr.assign]/9), which ends with the full-expression
  // ([class.temporary]).
  Place assign_class(At a) {
    auto s = get<ast::Assign>(a);
    if (s.op != ast::AssignOp::trivial_class_copy) unsupported(a, "without proven trivial copy assignment");
    At rhs = child(a, s.rhs);
    ast::QualType t = s.computation;
    std::optional<Place> temp;
    Place src;
    // A call's result is an object of its own, the temporary it initializes ([stmt.return]).
    if (is_lvalue(rhs)) {
      src = lvalue(rhs);
    } else if (is<ast::InitList>(rhs) || is<ast::ValueInit>(rhs)) {
      bool zeroed = value_initializes(rhs);
      temp = begin_object(t, zeroed ? ir::Init::zero : ir::Init::indeterminate, ir::Storage::temporary);
      initialize(*temp, rhs, zeroed);
      ValueId ops[] = {temp->ptr};
      std::uint64_t imms[] = {word(ty(t))};
      stateful(ir::Op::obj__begin, {}, ops, imms);
      src = *temp;
    } else {
      src = class_source(rhs);
    }
    Place p = lvalue(child(a, s.lhs));
    copy(p, src);
    if (temp) {
      ValueId ops[] = {temp->ptr};
      stateful(ir::Op::obj__end, {}, ops);
      std::uint64_t imms[] = {std::to_underlying(ir::Storage::temporary)};
      stateful(ir::Op::dealloc, {}, ops, imms);
    }
    return p;
  }

  // ------------------------------------------------------------------ state

  const ast::Interners& in_;
  const ast::Ast& ast_;
  const FunctionInput& fn_;
  F& facts_;
  ir::Builder b_;
  ir::FunctionDebug debug_;  // by emission id until `run` remaps it
  ir::BlockId block_{};
  ValueId state_{};
  bool live_ = false;
  uint32_t loc_ = 0;
  std::vector<std::vector<Ending>> scopes_;
  std::vector<std::uint32_t> scope_ids_;  // parallel to scopes_, for jumps (`LabelInfo`)
  std::uint32_t next_scope_ = 0;
  std::vector<Loop> loops_;
  std::vector<Cases> cases_;                       // the switches being generated, innermost last
  std::map<ast::NameId, LabelInfo> labels_;
  std::vector<PendingGoto> gotos_;                 // before their labels
  std::vector<std::optional<Place>> slots_;  // by local index
  const std::set<uint32_t>* planned_bypasses_;
  ConstantByteCapture* capture_;
  std::set<uint32_t> bypassed_locals_;       // discovered from scope transitions during first emission
  std::map<uint32_t, Place> jump_bindings_;  // local index to the address of its current instance
  std::optional<ValueId> this_;              // a non-static member function's `this`
  ast::EntityId pending_constructor_table_ = ast::EntityId::none;  // installed before the first member
  std::optional<Place> result_;              // the object a class result initializes
  uint32_t byval_params_ = 0, result_param_ = 0;  // as ir::Function's
  std::vector<std::vector<Ending>> temps_;   // the open full-expressions' temporaries
  std::vector<At> epilogue_;                 // a destructor's DestroyMember statements
  std::optional<Place> initialized_member_;  // a constructor's exceptional cleanup only
  int branches_ = 0;                         // conditionally evaluated operands entered
  bool selecting_initializer_ = false;      // class initializer selection and its cleanup
  bool selecting_return_ = false;           // a return selection requires checked class copies
  bool in_aggregate_default_ = false;        // evaluating a selected default member initializer
  int pinned_ = 0;                           // depth inside trees from other items (`Position`)
  uint32_t source_line_ = 0;                 // outermost active default argument's invocation line
  ast::ConstId source_descriptor_ = ast::ConstId::none;  // its descriptor address
};

template <Facts F>
std::expected<FunctionIr, std::string> function_ir(const ast::Interners& in, const FunctionInput& fn, F& facts) {
  // BOOTSTRAP: contracts: `pre` on a function template is dropped when it is instantiated through
  // an import (STATUS), so the precondition is asserted here. A dependent tree cannot reach IR
  // generation (ast.md §3.7); the check joins this one once #33 has dependent kinds.
  contract_assert(in.ast.contains(fn.body.tree));
  return Gen<F>(in, fn, facts).run();
}

template <Facts F>
std::expected<FunctionIr, std::string> constant_byte_capture_ir(const ast::Interners& in, const FunctionInput& fn,
                                                               F& facts, ConstantByteCapture& capture) {
  contract_assert(fn.thunk && fn.entity == ast::EntityId::none);
  return Gen<F>(in, fn, facts, nullptr, &capture).run();
}

}  // namespace ycxx::irgen
