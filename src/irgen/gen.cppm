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
// The emission order is the tree's evaluation order; `ir::Builder::finish` puts the body in
// canonical order (§2.4), so the fingerprint does not depend on it.
//
// α slice (#39): scalars, pointers and string literals, no classes, cleanups or exceptions.
// Calls are `call`, never `invoke`: with no destructors to run, an exception leaves the function
// with nothing to do. A call site will choose `invoke` once a scope has cleanups (§3.7.3); every
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
};

// A parameter's id in `FunctionDebug::vars`: parameters are not body locals, so they are numbered
// from here, in declaration order.
constexpr std::uint32_t param_debug_base = 1u << 31;

struct FunctionIr {
  ir::Function body;
  ir::FunctionDebug debug;  // keyed by `body`'s node ids
};

// Fails, naming the construct, on what the α slice does not generate yet.
template <Facts F>
std::expected<FunctionIr, std::string> function_ir(const ast::Interners& in, const FunctionInput& fn, F& facts);

}  // namespace ycxx::irgen

namespace ycxx::irgen {

using std::uint32_t;
using ast::NodeId;
using ir::ValueId;

// An object designated by an lvalue: a pointer to it and its type. Access paths are empty in the
// α slice, where every object is a complete scalar or array object.
struct Place {
  ValueId ptr{};
  ast::QualType type{};
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
    return out;
  }

 private:
  // ------------------------------------------------------------------ handles and types

  ir::QualType ty(ast::QualType q) { return ir::QualType(std::to_underlying(facts_.canonical(q))); }
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
    debug_.loc[ir::raw(n)] = ir::Loc{std::to_underlying(fn_.item), loc_};
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

  ValueId load(const Place& p) {
    ir::Ty r[] = {vty(p.type)};
    ValueId ops[] = {state_, p.ptr};
    std::uint64_t imms[] = {word(ty(p.type)), 0};  // the type, then an empty path
    return b_.result(emit(ir::Op::load, r, ops, imms));
  }
  void store(const Place& p, ValueId v) {
    ValueId ops[] = {p.ptr, v};
    std::uint64_t imms[] = {word(ty(p.type)), 0};
    stateful(ir::Op::store, {}, ops, imms);
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
  void br(ir::BlockId to, std::initializer_list<ValueId> values = {}) {
    ir::SuccRef s[] = {edge(to, values)};
    emit(ir::Op::br, {}, {}, {}, 0, s);
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

  Place begin_object(ast::QualType type, ir::Init init) {
    ir::Ty r[] = {vty(facts_.pointer_to(type))};
    std::uint64_t imms[] = {word(ty(type)), std::to_underlying(ir::Storage::automatic), std::to_underlying(init)};
    ir::NodeId n = stateful(ir::Op::alloc, r, {}, imms);
    return {b_.result(n, 1), type};
  }
  void start_lifetime(const Place& p) {
    ValueId ops[] = {p.ptr};
    std::uint64_t imms[] = {word(ty(p.type))};
    stateful(ir::Op::obj__begin, {}, ops, imms);
    scopes_.back().push_back(p);
  }
  void end_lifetime(const Place& p) {
    ValueId ops[] = {p.ptr};
    stateful(ir::Op::obj__end, {}, ops);
    std::uint64_t imms[] = {std::to_underlying(ir::Storage::automatic)};
    stateful(ir::Op::dealloc, {}, ops, imms);
  }
  // Ends the objects of the scopes above `depth`, innermost first; the scopes stay open.
  void exit_scopes(std::size_t depth) {
    for (std::size_t i = scopes_.size(); i-- > depth;)
      for (auto p = scopes_[i].rbegin(); p != scopes_[i].rend(); ++p) end_lifetime(*p);
  }
  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() {
    if (live_) exit_scopes(scopes_.size() - 1);
    scopes_.pop_back();
  }

  // ------------------------------------------------------------------ the function

  void body() {
    ast::QualType fty = fn_.decl.type;
    auto params = in_.seqs.elements(fn_.decl.params);
    std::vector<ir::Ty> entry{ir::Ty::state()};
    for (const ast::ParamSema& p : params) entry.push_back(vty(p.type));
    enter(b_.add_block(entry));
    loc_ = fn_.body_offset;
    push_scope();
    for (uint32_t i = 0; i < params.size(); ++i) {
      Place p = begin_object(params[i].type, ir::Init::indeterminate);
      debug_.vars.push_back({ir::LocalId(param_debug_base + i), {last_node()}});
      store(p, b_.param(block_, i + 1));
      start_lifetime(p);
      if (params[i].entity != ast::EntityId::none) params_.emplace_back(params[i].entity, p);
    }
    slots_.assign(in_.seqs.size(fn_.body.locals), std::nullopt);
    stmt({fn_.body.tree, fn_.body_offset});
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
  struct Position {
    uint32_t& loc;
    uint32_t saved;
    Position(uint32_t& l, uint32_t at) : loc(l), saved(l) { loc = at; }
    ~Position() { loc = saved; }
  };

  // A substatement is in a scope of its own even if it is not a compound statement ([stmt.pre]).
  void substmt(At a) {
    push_scope();
    stmt(a);
    pop_scope();
  }

  void stmt(At a) {
    if (a.id == NodeId::none) return;
    ensure_live();
    Position here(loc_, a.offset);
    if (is<ast::Compound>(a)) {
      auto s = get<ast::Compound>(a);
      push_scope();
      for (ast::Child c : ast_.children(s.stmts)) stmt(child(a, c));
      pop_scope();
    } else if (is<ast::ExprStmt>(a)) {
      discard(child(a, get<ast::ExprStmt>(a).expr));
    } else if (is<ast::Null>(a)) {
    } else if (is<ast::LocalVar>(a)) {
      local_var(a);
    } else if (is<ast::Return>(a)) {
      auto r = get<ast::Return>(a);
      if (in_.seqs.size(r.cleanups)) unsupported(a, "with cleanups");
      std::optional<ValueId> v;
      if (r.value.id != NodeId::none) v = rvalue(child(a, r.value));
      ret_(v);
    } else if (is<ast::If>(a)) {
      if_(a);
    } else if (is<ast::While>(a)) {
      auto w = get<ast::While>(a);
      loop(a, ast::Child{}, w.condition, ast::Child{}, w.body, false);
    } else if (is<ast::Do>(a)) {
      auto d = get<ast::Do>(a);
      loop(a, ast::Child{}, d.condition, ast::Child{}, d.body, true);
    } else if (is<ast::For>(a)) {
      auto f = get<ast::For>(a);
      loop(a, f.init, f.condition, f.increment, f.body, false);
    } else if (is<ast::Break>(a) || is<ast::Continue>(a)) {
      bool brk = is<ast::Break>(a);
      auto cleanups = brk ? get<ast::Break>(a).cleanups : get<ast::Continue>(a).cleanups;
      if (in_.seqs.size(cleanups)) unsupported(a, "with cleanups");
      if (loops_.empty()) unsupported(a, "outside a loop");
      const Loop& l = loops_.back();
      exit_scopes(l.depth);
      br(brk ? l.exit : l.next);
    } else {
      unsupported(a);
    }
  }

  void local_var(At a) {
    auto v = get<ast::LocalVar>(a);
    ast::LocalRec rec = in_.seqs.at(fn_.body.locals, v.local);
    if (rec.entity != ast::EntityId::none) unsupported(a, "with static or thread storage duration");
    bool has_init = v.init.id != NodeId::none;
    Place p = begin_object(rec.type, has_init ? ir::Init::indeterminate : ir::Init::erroneous);
    debug_.vars.push_back({ir::LocalId(v.local), {last_node()}});
    // The name is in scope in its own initializer ([basic.scope.pdecl]); reading it there reads an
    // object whose lifetime has not begun.
    slots_[v.local] = p;
    if (has_init) {
      At init = child(a, v.init);
      if (cls(rec.type) == TypeClass::array || cls(rec.type) == TypeClass::other)
        unsupported(a, "of aggregate type");
      store(p, rvalue(init));
    }
    start_lifetime(p);
  }

  void if_(At a) {
    auto s = get<ast::If>(a);
    push_scope();
    stmt(child(a, s.init));
    ValueId c = rvalue(child(a, s.condition));
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

  struct Loop {
    ir::BlockId next, exit;  // where `continue` and `break` go
    std::size_t depth;       // the scopes open outside the body
  };

  // `while`, `do` and `for` ([stmt.iter]): the condition block, the body, then for `for` the
  // increment block, which `continue` reaches.
  void loop(At a, ast::Child init, ast::Child cond, ast::Child incr, ast::Child body, bool body_first) {
    push_scope();  // a for-init-statement's scope ([stmt.for])
    if (init.id != NodeId::none) stmt(child(a, init));
    ir::BlockId head = new_block(), body_b = new_block(), exit = new_block();
    ir::BlockId next = incr.id != NodeId::none ? new_block() : head;
    br(body_first ? body_b : head);
    enter(head);
    if (cond.id != NodeId::none) {
      ValueId c = rvalue(child(a, cond));
      cond_br(c, edge(body_b), edge(exit));
    } else {
      br(body_b);
    }
    enter(body_b);
    loops_.push_back({next, exit, scopes_.size()});
    substmt(child(a, body));
    loops_.pop_back();
    if (live_) br(next);
    if (incr.id != NodeId::none) {
      enter(next);
      discard(child(a, incr));
      br(head);
    }
    enter(exit);
    pop_scope();
  }

  // ------------------------------------------------------------------ expressions

  ast::ExprInfo info(At a) {
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
  // writes or calls.
  int effects(At a) {
    if (a.id == NodeId::none) return 0;
    if (is<ast::Assign>(a) || is<ast::Call>(a)) return 2;
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
    Position here(loc_, a.offset);
    ast::ExprInfo ei = info(a);
    if (ei.category != ast::ValueCategory::prvalue) unsupported(a, "as a glvalue where a prvalue is expected");
    ast::QualType t = ei.type;
    if (is<ast::Literal>(a)) return constant(t, get<ast::Literal>(a).value);
    if (is<ast::Paren>(a)) return rvalue_or_void(child(a, get<ast::Paren>(a).operand));
    if (is<ast::ImplicitConversion>(a)) return conversion(a);
    if (is<ast::Unary>(a)) return unary(a);
    if (is<ast::Binary>(a)) return binary(a);
    if (is<ast::OrderedBinary>(a)) return ordered(a);
    if (is<ast::Call>(a)) return call(a);
    if (is<ast::Conditional>(a)) return conditional(a, false).value;
    unsupported(a);
  }

  std::optional<ValueId> conversion(At a) {
    auto c = get<ast::ImplicitConversion>(a);
    ast::QualType t = c.info.type;
    At from = child(a, c.operand);
    switch (c.conversion) {
      case ast::ConversionKind::lvalue_to_rvalue: return load(lvalue(from));
      case ast::ConversionKind::integral: {
        ValueId v = rvalue(from);
        if (same_type(info(from).type, t)) return v;
        return pure(ir::Op::iconv, t, {v});
      }
      case ast::ConversionKind::boolean: return pure(ir::Op::tobool, t, {rvalue(from)});
      case ast::ConversionKind::array_to_pointer: {
        Place arr = lvalue(from);
        ast::QualType elem = facts_.element(arr.type);
        std::uint64_t path[] = {1, ir::encode({ir::Step::elem, 0})};
        return pure(ir::Op::ptr_member, t, {arr.ptr}, {word(ty(elem)), path[0], path[1]});
      }
      case ast::ConversionKind::qualification: {
        ValueId v = rvalue(from);
        if (same_type(info(from).type, t)) return v;
        return pure(ir::Op::ptr_cast, t, {v}, {word(ty(t))});
      }
      case ast::ConversionKind::null_pointer:
        if (effects(from)) discard(from);  // a null pointer constant has no value to compute ([conv.ptr])
        return constant(t, facts_.null_constant(t));
      default: unsupported(a, "of this conversion kind");
    }
  }

  // `x + 1` or `x - 1` of `x`'s type, for `++` and `--` ([expr.pre.incr], [expr.post.incr]): the
  // arithmetic is done in the promoted type, so a narrow type wraps through the conversion back.
  ValueId step(ast::QualType t, ValueId old, bool inc) {
    if (cls(t) == TypeClass::pointer) {
      ValueId one = int_constant(facts_.int_type(), inc ? 1 : -1);
      return pure(ir::Op::ptr_add, t, {old, one}, {word(ty(facts_.pointee(t)))});
    }
    if (cls(t) != TypeClass::integer) throw Unsupported{"++ or -- of a non-integer type"};
    ast::QualType p = facts_.promoted(t);
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
        return pure(ir::Op::neg, t, {v}, {}, is_signed(t) ? ir::bit(ir::Flag::nsw) : 0);
      }
      case ast::UnaryOp::not_: return pure(ir::Op::xor_, t, {rvalue(o), int_constant(t, 1)});
      case ast::UnaryOp::compl_: return pure(ir::Op::xor_, t, {rvalue(o), int_constant(t, -1)});
      case ast::UnaryOp::address_of: return lvalue(o).ptr;
      case ast::UnaryOp::post_inc:
      case ast::UnaryOp::post_dec: {
        Place p = lvalue(o);
        ValueId old = load(p);
        store(p, step(p.type, old, u.op == ast::UnaryOp::post_inc));
        return old;
      }
      default: unsupported(a, "as a prvalue");
    }
  }

  // An arithmetic or bitwise operation on two values of the computation type `t`.
  ValueId arith(ast::BinaryOp op, ast::QualType t, ValueId x, ValueId y) {
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
    if (op == ast::BinaryOp::sub)
      i = pure(ir::Op::neg, it, {i}, {}, is_signed(it) ? ir::bit(ir::Flag::nsw) : 0);
    return pure(ir::Op::ptr_add, t, {p, i}, {word(ty(facts_.pointee(t)))});
  }

  std::optional<ValueId> binary(At a) {
    auto bn = get<ast::Binary>(a);
    At l = child(a, bn.lhs), r = child(a, bn.rhs);
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
        ValueId y = rvalue(r);
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
  std::optional<ValueId> call(At a) {
    auto c = get<ast::Call>(a);
    if (c.function == ast::EntityId::none) unsupported(a, "through a pointer");
    std::vector<ValueId> args;
    for (ast::Child arg : ast_.children(c.args)) args.push_back(rvalue(child(a, arg)));
    return call_fn(c.function, c.info.type, args);
  }
  std::optional<ValueId> call_fn(ast::EntityId f, ast::QualType result, std::span<const ValueId> args) {
    std::vector<ir::Ty> results;
    bool has_value = cls(result) != TypeClass::void_;
    if (has_value) results.push_back(vty(result));
    std::uint64_t imms[] = {word(f)};
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
      if (!has_value) {
        discard(x);
        br(join);
      } else {
        ValueId v = glvalue ? lvalue(x).ptr : rvalue(x);
        br(join, {v});
      }
    }
    enter(join);
    if (!has_value) return {};
    ValueId v = b_.param(join, 1);
    return {glvalue ? std::nullopt : std::optional(v), {v, t}};
  }

  // The object a glvalue designates ([basic.lval]).
  Place lvalue(At a) {
    Position here(loc_, a.offset);
    ast::ExprInfo ei = info(a);
    if (ei.category == ast::ValueCategory::prvalue) unsupported(a, "as a prvalue where a glvalue is expected");
    ast::QualType t = ei.type;
    if (is<ast::LocalRef>(a)) {
      uint32_t i = get<ast::LocalRef>(a).local;
      if (i >= slots_.size() || !slots_[i]) unsupported(a, "naming a local that is not declared yet");
      return *slots_[i];
    }
    if (is<ast::DeclRef>(a)) {
      ast::EntityId e = get<ast::DeclRef>(a).entity;
      for (auto& [pe, p] : params_)
        if (pe == e) return p;
      if (cls(t) == TypeClass::function) unsupported(a, "naming a function as an object");
      // A namespace-scope variable: its address is a constant of the entity ([basic.stc.static]).
      return {pure(ir::Op::global, facts_.pointer_to(t), {}, {word(e)}), t};
    }
    if (is<ast::Literal>(a)) {
      // A string literal names a static array object ([lex.string]).
      ast::ConstId addr = facts_.string_address(get<ast::Literal>(a).value);
      return {constant(facts_.pointer_to(t), addr), t};
    }
    if (is<ast::Paren>(a)) return lvalue(child(a, get<ast::Paren>(a).operand));
    if (is<ast::Unary>(a)) {
      auto u = get<ast::Unary>(a);
      At o = child(a, u.operand);
      if (u.op == ast::UnaryOp::deref) return {rvalue(o), t};
      if (u.op == ast::UnaryOp::pre_inc || u.op == ast::UnaryOp::pre_dec) {
        Place p = lvalue(o);
        store(p, step(p.type, load(p), u.op == ast::UnaryOp::pre_inc));
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
      ast::QualType it = info(rhs).type;
      ValueId i = s.op == ast::AssignOp::add ? v
                : pure(ir::Op::neg, it, {v}, {}, is_signed(it) ? ir::bit(ir::Flag::nsw) : 0);
      r = pure(ir::Op::ptr_add, p.type, {old, i}, {word(ty(facts_.pointee(p.type)))});
    } else {
      bool is_shift = s.op == ast::AssignOp::shl || s.op == ast::AssignOp::shr;
      ast::QualType ct = is_shift ? facts_.promoted(p.type) : info(rhs).type;
      ValueId x = same_type(ct, p.type) ? old : pure(ir::Op::iconv, ct, {old});
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
      if (!same_type(ct, p.type)) r = pure(ir::Op::iconv, p.type, {r});
    }
    store(p, r);
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
  std::vector<std::vector<Place>> scopes_;
  std::vector<Loop> loops_;
  std::vector<std::optional<Place>> slots_;  // by local index
  std::vector<std::pair<ast::EntityId, Place>> params_;
};

template <Facts F>
std::expected<FunctionIr, std::string> function_ir(const ast::Interners& in, const FunctionInput& fn, F& facts) {
  contract_assert(in.ast.contains(fn.body.tree));
  return Gen<F>(in, fn, facts).run();
}

}  // namespace ycxx::irgen
