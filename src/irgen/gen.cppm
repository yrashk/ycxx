// IR generation (optimization design §2, §11; ast.md §3.6–§3.7): `function_ir` turns the analysed
// body of a function into its L1 body, and its positions into `function_debug`.
//
// The shape follows L1's (§2.2): a CFG of blocks with arguments that threads one `!state` chain.
// - Every block but the entry takes the state as its first parameter; the entry takes the state,
//   then the function's parameters.
// - Every parameter and block-scope variable is an `alloc` with an init kind ([dcl.init.general],
//   P2795): `erroneous` without an initializer, else `indeterminate`. Its initializing store comes
//   before `obj.begin`, since a scalar's lifetime begins when its initialization is complete
//   ([basic.life]). Every scope exit (the end of a block, `return`, `break`, `continue`) emits
//   `obj.end` and `dealloc` for the scope's objects in reverse order of declaration.
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
//   of type T (array-to-pointer is `ptr_member [elem, /e0]`); `ptr_add`/`ptr_diff [T]` step by
//   elements of type T.
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
// Calls are `call`, never `invoke`: with no destructors to run, an exception leaves the function
// with nothing to do. A call site will choose `invoke` once a scope has destructors (§3.7.3); every
// call already goes through `call_fn` for that reason.
export module ycxx.irgen:gen;

import std;
import ycxx.ast;
import ycxx.ir;
import :facts;

export namespace ycxx::irgen {

namespace ir = ycxx::ir;

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
  // index. Each becomes a const object of the thunk, made at its entry; a local without a value
  // becomes an object outside its lifetime.
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
};

struct FunctionIr {
  ir::Function body;
  ir::FunctionDebug debug;  // keyed by `body`'s node ids
};

// Namespace-scope reference initialization cannot extend the lifetime of a temporary in the
// startup function's frame (#367).
bool materializes_temporary(const ast::Ast& ast, ast::NodeId n) {
  if (n == ast::NodeId::none) return false;
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
  Gen(const ast::Interners& in, const FunctionInput& fn, F& facts) : in_(in), ast_(in.ast), fn_(fn), facts_(facts) {}

  std::expected<FunctionIr, std::string> run() {
    try {
      body();
    } catch (const Unsupported& u) {
      return std::unexpected(std::format("function_ir: {} is not supported yet", u.what));
    }
    auto built = b_.finish();
    if (!built) return std::unexpected("function_ir: " + built.error());
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
  ValueId load(const Place& p) {
    ir::Ty r[] = {vty(p.type)};
    ValueId ops[] = {state_, p.ptr};
    if (p.bit_field) {
      std::uint64_t imms[] = {*p.bit_field};
      return b_.result(emit(ir::Op::bit_field_load, r, ops, imms));
    }
    std::uint64_t imms[] = {word(ty(p.type)), 0};  // the type, then an empty path
    return b_.result(emit(ir::Op::load, r, ops, imms, access_flags(p.type)));
  }
  void store(const Place& p, ValueId v) {
    ValueId ops[] = {p.ptr, v};
    if (p.bit_field) {
      std::uint64_t imms[] = {*p.bit_field};
      stateful(ir::Op::bit_field_store, {}, ops, imms);
      return;
    }
    std::uint64_t imms[] = {word(ty(p.type)), 0};
    stateful(ir::Op::store, {}, ops, imms, access_flags(p.type));
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
    std::uint64_t imms[] = {word(ty(type)), std::to_underlying(storage), std::to_underlying(init)};
    ir::NodeId n = stateful(ir::Op::alloc, r, {}, imms);
    return {b_.result(n, 1), type};
  }
  // An object a scope ends: its destructor (none if trivial) runs, then its lifetime and storage
  // end ([basic.life], [class.dtor]).
  struct Ending {
    Place place;
    ast::EntityId destructor = ast::EntityId::none;
    ir::Storage storage = ir::Storage::automatic;
  };
  void start_lifetime(const Place& p, ast::EntityId destructor = ast::EntityId::none,
                      ir::Storage storage = ir::Storage::automatic) {
    ValueId ops[] = {p.ptr};
    std::uint64_t imms[] = {word(ty(p.type))};
    stateful(ir::Op::obj__begin, {}, ops, imms);
    scopes_.back().push_back({p, destructor, storage});
  }
  void end_lifetime(const Ending& e) {
    if (e.destructor != ast::EntityId::none) destroy(e.place, e.destructor);
    ValueId ops[] = {e.place.ptr};
    stateful(ir::Op::obj__end, {}, ops);
    std::uint64_t imms[] = {std::to_underlying(e.storage)};
    stateful(ir::Op::dealloc, {}, ops, imms);
  }
  // Calls `destructor` on the object at `p`; an array's elements, the last first ([class.dtor]).
  void destroy(const Place& p, ast::EntityId destructor) {
    if (cls(p.type) == TypeClass::array) {
      ast::QualType e = facts_.element(p.type);
      for (std::uint64_t i = facts_.bound(p.type); i-- > 0;) destroy(subobject(p, ir::Step::elem, uint32_t(i), e), destructor);
      return;
    }
    ValueId self[] = {p.ptr};
    call_fn(destructor, facts_.int_type(), self, 1, true);
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
    for (auto t = ts.rbegin(); t != ts.rend(); ++t) end_lifetime(*t);
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
    // Static locals: their objects are their entities', wherever control enters ([stmt.dcl]/3).
    for (uint32_t i = params.size(); !fn_.thunk && i < slots_.size(); ++i) {
      ast::LocalRec rec = in_.seqs.at(fn_.body.locals, i);
      if (rec.entity != ast::EntityId::none && !facts_.is_reference(rec.type))
        slots_[i] = Place{pure(ir::Op::global, facts_.pointer_to(rec.type), {}, {word(rec.entity)}), rec.type};
    }
    // A thunk's locals: those with values are const objects; any other is an object whose
    // lifetime has ended, so evaluating a use of it fails, while naming it unevaluated does not.
    for (uint32_t i = 0; fn_.thunk && i < slots_.size(); ++i) {
      if (slots_[i]) continue;
      const ast::LocalRec rec = in_.seqs.at(fn_.body.locals, i);
      if (rec.flags & ast::local_flag::requires_parameter) continue;
      ast::QualType t = rec.type;
      // A class or array local is never usable in a constant expression: naming it is
      // unsupported, so it needs no object.
      if (cls(t) == TypeClass::record || cls(t) == TypeClass::array || facts_.is_reference(t) ||
          in_.types.is<ast::TMemberPtr>(in_.types.canon(t)) ||
          in_.types.is<ast::TEnum>(in_.types.canon(t)))
        continue;  // (the interpreter has no enumeration objects yet, #25)
      Place p = begin_object(t, ir::Init::indeterminate);
      if (i < fn_.local_values.size() && fn_.local_values[i]) {
        store(p, constant(t, *fn_.local_values[i]));
        start_lifetime(p);
      } else {
        ValueId ops[] = {p.ptr};
        std::uint64_t imms[] = {word(ty(p.type))};
        stateful(ir::Op::obj__begin, {}, ops, imms);
        stateful(ir::Op::obj__end, {}, ops);
      }
      slots_[i] = p;
    }
    stmt({fn_.body.tree, fn_.body_offset});
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

  bool is_literal(At a) {
    while (is<ast::Paren>(a)) a = child(a, get<ast::Paren>(a).operand);
    return is<ast::Literal>(a);
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

  // A substatement is in a scope of its own even if it is not a compound statement ([stmt.pre]).
  void substmt(At a) {
    push_scope();
    stmt(a);
    pop_scope();
  }

  void stmt(At a) {
    if (a.id == NodeId::none) return;
    ensure_live();
    Position here(*this, a.offset);
    if (is<ast::Compound>(a)) {
      auto s = get<ast::Compound>(a);
      push_scope();
      for (ast::Child c : ast_.children(s.stmts)) stmt(child(a, c));
      pop_scope();
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
    } else if (is<ast::Destructor>(a)) {
      auto d = get<ast::Destructor>(a);
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
      if (result_ && r.value.id != NodeId::none) {
        // A class result initializes the caller's object ([stmt.return], [dcl.init.general]/16.6).
        full_expression([&] {
          initialize(*result_, value, false);
          return 0;
        });
      } else if (r.value.id != NodeId::none) {
        v = full_expression([&] { return is_lvalue(value) ? std::optional(address(value)) : rvalue_or_void(value); });
      }
      ret_(v);
    } else if (is<ast::If>(a)) {
      if_(a);
    } else if (is<ast::IfConsteval>(a)) {
      if_consteval(a);
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
    bool zeroed = has_init && value_initializes(child(a, v.init));
    bool partial = has_init && !zeroed && default_initializes(child(a, v.init));
    Place p = begin_object(rec.type, zeroed ? ir::Init::zero
                                     : has_init && !partial ? ir::Init::indeterminate
                                                            : ir::Init::erroneous);
    debug_.vars.push_back({ir::LocalId(v.local), {last_node()}});
    // The name is in scope in its own initializer ([basic.scope.pdecl]); reading it there reads an
    // object whose lifetime has not begun.
    slots_[v.local] = p;
    if (has_init)
      full_expression([&] {
        initialize(p, child(a, v.init), zeroed);
        return 0;
      });
    start_lifetime(p, v.destructor);
  }

  // Whether initializer `e` value-initializes some element (`ValueInit`), so the object is
  // allocated zero.
  bool value_initializes(At e) {
    if (is<ast::ValueInit>(e)) return true;
    if (is<ast::CtorCall>(e)) return get<ast::CtorCall>(e).zero_first != 0;
    if (is<ast::Paren>(e)) return value_initializes(child(e, get<ast::Paren>(e).operand));
    if (is<ast::DefaultArg>(e)) return value_initializes(child(e, get<ast::DefaultArg>(e).arg));
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

  // Initializes the object at `p` from the prvalue `e` ([dcl.init]). In a `zeroed` object, a
  // value-initialized element is already zero.
  void initialize(const Place& p, At e, bool zeroed) {
    Position here(*this, e.offset);
    if (is<ast::Paren>(e)) return initialize(p, child(e, get<ast::Paren>(e).operand), zeroed);
    if (is<ast::DefaultArg>(e)) return pinned([&] { initialize(p, child(e, get<ast::DefaultArg>(e).arg), zeroed); });
    if (facts_.is_reference(p.type)) {
      store(p, address(e));  // a reference member holds its object's address
      return;
    }
    if (is<ast::ImplicitConversion>(e)) {
      auto c = get<ast::ImplicitConversion>(e);
      if (c.conversion == ast::ConversionKind::array_copy)
        return copy_array(p, lvalue(child(e, c.operand)));
    }
    if (is<ast::Call>(e) && class_prvalue(info(e))) {
      call(e, p.ptr);  // the callee initializes the object ([stmt.return])
      return;
    }
    if (is<ast::MemberCall>(e) && class_prvalue(info(e))) {
      member_call(e, p.ptr);
      return;
    }
    if (is<ast::CtorCall>(e)) {
      // [class.ctor]: the constructor initializes the object in place.
      auto c = get<ast::CtorCall>(e);
      if (c.zero_first && !zeroed) unsupported(e, "value-initializing an object that is not new");
      std::vector<ValueId> args{p.ptr};
      uint32_t byval = 0;
      for (ast::Child arg : ast_.children(c.args)) {
        At x = child(e, arg);
        if (argument_object(x)) byval |= 1u << args.size();
        args.push_back(argument(x));
      }
      call_fn(c.ctor, facts_.int_type(), args, 1, true, byval);
      return;
    }
    if (is<ast::InitList>(e)) {
      bool array = cls(p.type) == TypeClass::array;
      // A class's elements are its direct bases, then its members ([dcl.init.aggr]/2).
      std::vector<ast::EntityId> bases = array ? std::vector<ast::EntityId>{} : facts_.bases_of(facts_.class_entity(p.type));
      uint32_t i = 0;
      for (ast::Child c : ast_.children(get<ast::InitList>(e).elems)) {
        At x = child(e, c);
        if (i < bases.size()) initialize(subobject(p, ir::Step::base, std::to_underlying(bases[i]), info(x).type), x, zeroed);
        else initialize(subobject(p, array ? ir::Step::elem : ir::Step::field, i - uint32_t(bases.size()), info(x).type), x, zeroed);
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
      if (cls(p.type) == TypeClass::record || cls(p.type) == TypeClass::array) unsupported(e, "of an aggregate");
      store(p, zero(p.type));
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
  Place materialize(At a) {
    auto m = get<ast::Materialize>(a);
    At v = child(a, m.operand);
    bool zeroed = value_initializes(v);
    Place p = begin_object(m.info.type, zeroed ? ir::Init::zero : ir::Init::indeterminate, ir::Storage::temporary);
    initialize(p, v, zeroed);
    ValueId ops[] = {p.ptr};
    std::uint64_t imms[] = {word(ty(p.type))};
    stateful(ir::Op::obj__begin, {}, ops, imms);
    Ending end{p, m.destructor, ir::Storage::temporary};
    if (m.extended == 1) scopes_.back().push_back(end);
    else if (temps_.empty()) unsupported(a, "outside a full-expression");
    else if (branches_ > 0 && m.destructor != ast::EntityId::none) unsupported(a, "with a destructor, created conditionally");
    else temps_.back().push_back(end);
    return p;
  }

  // The object a class prvalue is copied from: a glvalue of the class converted to a prvalue
  // ([conv.lval]), whose copy constructor is trivial.
  Place class_source(At e) {
    while (is<ast::Paren>(e) || is<ast::DefaultArg>(e))
      e = child(e, is<ast::Paren>(e) ? get<ast::Paren>(e).operand : get<ast::DefaultArg>(e).arg);
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
  ValueId zero(ast::QualType t) {
    if (in_.types.is<ast::TMemberPtr>(in_.types.canon(t))) throw Unsupported{"value-initializing a pointer to member"};
    if (cls(t) == TypeClass::pointer) return constant(t, facts_.null_constant(t));
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
    emit(ir::Op::consteval_br, {}, {}, {}, 0, succs);
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
      call_fn(atexit_fn, facts_.int_type(), args, 0);
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

  // [stmt.dcl]/3: a static local's object is its entity's. Unless constant-initialized with
  // nothing to destroy, it is initialized the first time control passes here, once even with
  // several threads: `if (__cxa_guard_acquire(&guard)) { init; atexit(destructor); __cxa_guard_release(&guard); }`
  // (Itanium C++ ABI §3.3.3), the acquire called on every pass.
  void static_var(At a) {
    auto s = get<ast::StaticVar>(a);
    ast::LocalRec rec = in_.seqs.at(fn_.body.locals, s.local);
    if (s.destructor == ast::EntityId::none && (s.value != ast::ConstId::none || s.init.id == NodeId::none)) return;
    auto g = std::ranges::find(fn_.guards, rec.entity, &FunctionInput::StaticGuard::variable);
    if (g == fn_.guards.end() || fn_.guard_acquire == ast::EntityId::none) {
      // For constant evaluation, which has no guards: passing here is not constant ([expr.const]).
      emit(ir::Op::unreachable, {}, {}, {}, ir::bit(ir::Flag::not_constexpr));
      live_ = false;
      return;
    }
    // Keep the address defined at function entry so jumps may bypass this declaration.
    const Place& p = *slots_[s.local];
    ast::QualType vp = facts_.pointer_to(facts_.int_type());
    ValueId guard[] = {pure(ir::Op::global, vp, {}, {word(g->guard)})};
    ValueId first = *call_fn(fn_.guard_acquire, facts_.int_type(), guard, 0);
    ValueId taken = pure(ir::Op::icmp, facts_.bool_type(), {first, int_constant(facts_.int_type(), 0)}, {std::to_underlying(ir::Pred::ne)});
    ir::BlockId init_b = new_block(), join = new_block();
    cond_br(taken, edge(init_b), edge(join));
    enter(init_b);
    if (s.init.id != NodeId::none && s.value == ast::ConstId::none)
      full_expression([&] {
        initialize(p, child(a, s.init), false);
        return 0;
      });
    if (s.destructor != ast::EntityId::none) at_exit(p, s.destructor, fn_.atexit, fn_.dso);
    call_fn(fn_.guard_release, facts_.int_type(), guard, 0, true);
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
    // Sema rejects a jump past an initialization ([stmt.dcl]/2); one past a vacuous one would leave
    // the object without storage on this path.
    for (std::size_t d = cs.depth; d < scopes_.size(); ++d)
      if (!scopes_[d].empty()) unsupported(a, "past a declaration");
    if (live_) br(b->second);
    enter(b->second);
    stmt(child(a, is<ast::Case>(a) ? get<ast::Case>(a).stmt : get<ast::Default>(a).stmt));
  }

  // Labels and `goto` ([stmt.label], [stmt.goto]). A jump ends the objects of the scopes it
  // leaves, and, in the scopes it stays in, those begun after the label ([stmt.jump]/2). A goto
  // before its label branches to a block of its own, filled once the label is placed.
  struct LabelInfo {
    ir::BlockId block;
    bool placed = false;
    std::vector<std::uint32_t> path;  // the ids of the scopes open at the label, outermost first
    std::vector<std::size_t> counts;  // how many objects each had begun there
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
  void jump_exits(At a, std::span<const std::uint32_t> path, std::span<const std::vector<Ending>> scopes, const LabelInfo& l) {
    std::size_t k = 0;
    while (k < path.size() && k < l.path.size() && path[k] == l.path[k]) ++k;
    // Entering a scope past an object: as for `case_label`.
    for (std::size_t d = 0; d < l.path.size(); ++d)
      if (l.counts[d] > (d < k ? scopes[d].size() : 0)) unsupported(a, "past a declaration");
    for (std::size_t i = scopes.size(); i-- > 0;)
      for (std::size_t j = scopes[i].size(); j-- > (i < k ? l.counts[i] : 0);) end_lifetime(scopes[i][j]);
  }
  void goto_(At a) {
    auto g = get<ast::Goto>(a);
    LabelInfo& l = label_info(g.label);
    if (l.placed) {
      jump_exits(a, scope_ids_, scopes_, l);
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
    l.counts.clear();
    for (const auto& sc : scopes_) l.counts.push_back(sc.size());
    ValueId state = state_;
    for (auto g = gotos_.begin(); g != gotos_.end();) {
      if (g->label != s.name) {
        ++g;
        continue;
      }
      enter(g->from);
      Position at(*this, g->loc);
      jump_exits(g->at, g->path, g->scopes, l);
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
    if (is_lvalue(a)) lvalue(a);
    else rvalue_or_void(a);
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
    if (a.id == NodeId::none) return 0;
    if (is<ast::Assign>(a)) return 2;
    if (is<ast::Unary>(a)) {
      auto op = get<ast::Unary>(a).op;
      if (op == ast::UnaryOp::pre_inc || op == ast::UnaryOp::pre_dec || op == ast::UnaryOp::post_inc ||
          op == ast::UnaryOp::post_dec)
        return 2;
    }
    int e = is<ast::ImplicitConversion>(a) &&
                    get<ast::ImplicitConversion>(a).conversion == ast::ConversionKind::lvalue_to_rvalue
                ? 1
                : 0;
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
    if (is<ast::Literal>(a)) return constant(t, get<ast::Literal>(a).value);
    if (is<ast::Paren>(a)) return rvalue_or_void(child(a, get<ast::Paren>(a).operand));
    if (is<ast::DefaultArg>(a)) return pinned([&] { return rvalue_or_void(child(a, get<ast::DefaultArg>(a).arg)); });
    if (is<ast::ImplicitConversion>(a)) return conversion(a);
    if (is<ast::Unary>(a)) return unary(a);
    if (is<ast::Binary>(a)) return binary(a);
    if (is<ast::OrderedBinary>(a)) return ordered(a);
    if (is<ast::Call>(a)) return call(a);
    if (is<ast::MemberCall>(a)) return member_call(a);
    if (is<ast::BuiltinCall>(a)) {  // `__builtin_fabs` (lex::BuiltinFunction::fabs), the one there is
      auto b = get<ast::BuiltinCall>(a);
      return pure(ir::Op::fabs, t, {rvalue(child(a, *ast_.children(b.args).begin()))});
    }
    if (is<ast::This>(a)) {
      if (!this_) unsupported(a, "outside a non-static member function");
      return *this_;
    }
    if (is<ast::Conditional>(a)) return conditional(a, false).value;
    if (is<ast::ValueInit>(a) && cls(t) != TypeClass::record && cls(t) != TypeClass::array) return zero(t);
    unsupported(a);
  }

  std::optional<ValueId> conversion(At a) {
    auto c = get<ast::ImplicitConversion>(a);
    ast::QualType t = c.info.type;
    At from = child(a, c.operand);
    switch (c.conversion) {
      case ast::ConversionKind::lvalue_to_rvalue: return load(lvalue(from));
      case ast::ConversionKind::function_to_pointer: return lvalue(from).ptr;
      case ast::ConversionKind::integral:
      case ast::ConversionKind::floating:
      case ast::ConversionKind::floating_integral: return arithmetic(info(from).type, t, rvalue(from));
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
      case ast::ConversionKind::to_void:
        discard(from);
        return std::nullopt;
      case ast::ConversionKind::null_pointer:
        if (effects(from)) discard(from);  // a null pointer constant has no value to compute ([conv.ptr])
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
    // Two std::nullptr_t values are equal ([expr.eq]/7). Such an operand is a `nullptr` literal
    // in the α subset, so nothing is evaluated.
    if (cls(info(l).type) == TypeClass::other && (bn.op == ast::BinaryOp::eq || bn.op == ast::BinaryOp::ne)) {
      for (At o : {l, r})
        if (!is_literal(o)) unsupported(o, "of type std::nullptr_t");
      return int_constant(bn.info.type, bn.op == ast::BinaryOp::eq);
    }
    ValueId x{}, y{};
    unsequenced(std::array{l, r}, [&](std::size_t i, At o) { (i ? y : x) = rvalue(o); });
    ast::QualType t = bn.info.type, ot = info(l).type;
    using enum ast::BinaryOp;
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
      case spaceship: unsupported(a, "<=>");
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
        ir::BlockId rhs_b = new_block(), join = new_block({t});
        if (is_and) cond_br(x, edge(rhs_b), edge(join, {x}));
        else cond_br(x, edge(join, {x}), edge(rhs_b));
        enter(rhs_b);
        ++branches_;
        ValueId y = rvalue(r);
        --branches_;
        br(join, {y});
        enter(join);
        return b_.param(join, 1);
      }
      case ast::OrderedOp::comma:
        discard(l);
        return rvalue_or_void(r);
      default: unsupported(a, "pointer-to-member access");
    }
  }

  // A call to a named function ([expr.call]). The callee is not evaluated: naming a function has
  // no effect. Arguments are evaluated in order, which is one of the indeterminately sequenced
  // orders the clause allows.
  std::optional<ValueId> call(At a, std::optional<ValueId> into = std::nullopt) {
    auto c = get<ast::Call>(a);
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
    uint32_t byval = 0;
    for (ast::Child arg : ast_.children(c.args)) {
      At x = child(a, arg);
      // An argument object of class type: sema materialized it ([expr.call]/7).
      if (argument_object(x)) byval |= 1u << args.size();
      args.push_back(argument(x));
    }
    if (through) return call_pointer(*through, result_type(c.info), args, byval, into ? 1 : 0);
    return call_fn(c.function, result_type(c.info), args, 0, false, byval, into ? 1 : 0);
  }
  // A call through a function pointer, `args[0]`, of function type `fn`: `call_fn`'s `call_ind`.
  // The masks count the pointer as no argument.
  std::optional<ValueId> call_pointer(ast::QualType fn, ast::QualType result, std::span<const ValueId> args, uint32_t byval,
                                      uint32_t result_arg) {
    std::vector<ir::Ty> results;
    if (args.size() > 33) throw Unsupported{"a call with more than 32 arguments"};
    bool has_value = !result_arg && cls(result) != TypeClass::void_;
    if (has_value) results.push_back(vty(result));
    auto f = in_.types.get<ast::TFunction>(in_.types.canon(fn));
    std::size_t named = in_.seqs.size(f.params), passed = args.size() - 1 - (result_arg ? 1 : 0);
    std::size_t through_ellipsis = f.variadic && passed > named ? passed - named : 0;
    std::uint64_t imms[] = {through_ellipsis, byval >> 1, result_arg};
    ir::NodeId n = stateful(ir::Op::call_ind, results, args, imms);
    if (has_value) return b_.result(n, 1);
    return std::nullopt;
  }
  // A non-static member function's call: the object's address is the first argument.
  std::optional<ValueId> member_call(At a, std::optional<ValueId> into = std::nullopt) {
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
    return call_fn(c.function, result_type(c.info), args, 1, false, byval, into ? 2 : 0);
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
  // A call to `f`. The first `implicit` arguments are not its parameters' (the object); the
  // arguments after its parameters are the ones its ellipsis matches ([expr.call]/12), which
  // sema already promoted.
  // `byval` and `result_arg` are the call op's masks (ir/ops.cppm); a class result is written
  // through the argument `result_arg` names and gives no value.
  std::optional<ValueId> call_fn(ast::EntityId f, ast::QualType result, std::span<const ValueId> args,
                                 std::size_t implicit, bool void_ = false, uint32_t byval = 0, uint32_t result_arg = 0) {
    std::vector<ir::Ty> results;
    if (args.size() > 32) throw Unsupported{"a call with more than 32 arguments"};
    bool has_value = !void_ && !result_arg && cls(result) != TypeClass::void_;
    if (has_value) results.push_back(vty(result));
    std::size_t through_ellipsis = 0;
    std::size_t hidden = implicit + (result_arg ? 1 : 0);
    if (auto named = facts_.ellipsis_after(f); named && args.size() > hidden + *named)
      through_ellipsis = args.size() - hidden - *named;
    std::uint64_t imms[] = {word(f), through_ellipsis, byval, result_arg};
    ir::Flags flags = facts_.is_constexpr(f) ? 0 : ir::bit(ir::Flag::not_constexpr);
    ir::NodeId n = stateful(ir::Op::call, results, args, imms, flags);
    if (has_value) return b_.result(n, 1);
    return std::nullopt;
  }

  // `c ? x : y` ([expr.cond]): a join with the chosen value, or for a glvalue the chosen object.
  struct Chosen {
    std::optional<ValueId> value;
    Place place;
  };
  Chosen conditional(At a, bool glvalue) {
    auto c = get<ast::Conditional>(a);
    ast::QualType t = c.info.type;
    bool has_value = glvalue || cls(t) != TypeClass::void_;
    ast::QualType jt = glvalue ? facts_.pointer_to(t) : t;
    ValueId cv = rvalue(child(a, c.condition));
    ir::BlockId then_b = new_block(), else_b = new_block();
    ir::BlockId join = has_value ? new_block({jt}) : new_block();
    cond_br(cv, edge(then_b), edge(else_b));
    for (auto [blk, arm] : {std::pair{then_b, c.then_}, std::pair{else_b, c.else_}}) {
      enter(blk);
      At x = child(a, arm);
      ++branches_;
      if (!has_value) {
        discard(x);
        br(join);
      } else {
        ValueId v = glvalue ? address(x) : rvalue(x);
        br(join, {v});
      }
      --branches_;
    }
    enter(join);
    if (!has_value) return {};
    ValueId v = b_.param(join, 1);
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
      if (i >= slots_.size() || !slots_[i]) unsupported(a, "naming a local that is not declared yet");
      return *slots_[i];
    }
    // A derived class's object as its direct base's ([conv.ptr]/3, [dcl.init.ref]): the base
    // subobject, named by the base class's entity.
    if (is<ast::ImplicitConversion>(a) && get<ast::ImplicitConversion>(a).conversion == ast::ConversionKind::derived_to_base) {
      auto c = get<ast::ImplicitConversion>(a);
      return subobject(lvalue(child(a, c.operand)), ir::Step::base, std::to_underlying(c.function), t);
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
    if (is<ast::Call>(a) || is<ast::MemberCall>(a)) {
      auto v = is<ast::Call>(a) ? call(a) : member_call(a);
      return {*v, t};  // a call returning a reference designates the object it refers to
    }
    if (is<ast::Assign>(a)) return assign(a);
    if (is<ast::Conditional>(a)) return conditional(a, true).place;
    if (is<ast::OrderedBinary>(a) && get<ast::OrderedBinary>(a).op == ast::OrderedOp::comma) {
      auto o = get<ast::OrderedBinary>(a);
      discard(child(a, o.lhs));
      return lvalue(child(a, o.rhs));
    }
    unsupported(a, "as a glvalue");
  }

  // `a = b` and `a op= b` ([expr.assign]): the right operand is sequenced before the left. In a
  // compound assignment the right operand already has the computation type (for shifts, its own
  // promoted type); the left is loaded once, converted to it, and the result converted back.
  Place assign(At a) {
    auto s = get<ast::Assign>(a);
    if (cls(s.info.type) == TypeClass::record) return assign_class(a);
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
    At rhs = child(a, s.rhs);
    ast::QualType t = s.computation;
    std::optional<Place> temp;
    Place src;
    // A call's result is an object of its own, the temporary it initializes ([stmt.return]).
    if (is<ast::Materialize>(strip_parens(rhs))) {
      // A call's result: the temporary sema made of it, ended with its destructor.
      Place p = lvalue(child(a, s.lhs));
      copy(p, materialize(strip_parens(rhs)));
      return p;
    }
    if (is<ast::InitList>(rhs) || is<ast::ValueInit>(rhs)) {
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
  std::optional<ValueId> this_;              // a non-static member function's `this`
  std::optional<Place> result_;              // the object a class result initializes
  uint32_t byval_params_ = 0, result_param_ = 0;  // as ir::Function's
  std::vector<std::vector<Ending>> temps_;   // the open full-expressions' temporaries
  std::vector<At> epilogue_;                 // a destructor's DestroyMember statements
  int branches_ = 0;                         // conditionally evaluated operands entered
  int pinned_ = 0;                           // depth inside trees from other items (`Position`)
};

template <Facts F>
std::expected<FunctionIr, std::string> function_ir(const ast::Interners& in, const FunctionInput& fn, F& facts) {
  // BOOTSTRAP: contracts: `pre` on a function template is dropped when it is instantiated through
  // an import (STATUS), so the precondition is asserted here. A dependent tree cannot reach IR
  // generation (ast.md §3.7); the check joins this one once #33 has dependent kinds.
  contract_assert(in.ast.contains(fn.body.tree));
  return Gen<F>(in, fn, facts).run();
}

}  // namespace ycxx::irgen
