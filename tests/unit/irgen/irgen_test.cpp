// Unit tests of IR generation (#24): analysed bodies built with the AST builder, as sema (#41)
// will build them, go through `function_ir`; each result must pass `verify`, and its text form,
// with types, entities and constants named, must equal the expected one.
import std;
import ycxx.ast;
import ycxx.ir;
import ycxx.irgen;
import ycxx.unit;

namespace ast = ycxx::ast;
namespace ir = ycxx::ir;
using namespace ycxx::irgen;
using ast::Child;
using ast::ConstId;
using ast::EntityId;
using ast::NodeId;
using ast::QualType;
using ycxx::unit::check;

namespace {

// ------------------------------------------------------------------ facts

// Types and integer constants are the real ones (#33), through `TypeFacts`. Entities and string
// literals are stand-ins until the interners hold them: an entity is a name, and a string
// literal's constant is a numbered token whose address is another token.
struct TestFacts : TypeFacts {
  using TypeFacts::TypeFacts;
  std::map<std::uint32_t, std::string> tokens, entities;
  std::set<std::uint32_t> constexpr_fns;
  std::map<std::uint32_t, std::uint32_t> variadic_fns;  // parameters before the ellipsis
  EntityId main_fn = EntityId::none;

  EntityId entity(std::string name) {
    EntityId e = interners().entities.add<EntityId>(ast::seed("entity " + name));
    entities[std::to_underlying(e)] = name;
    return e;
  }
  ConstId token(std::string spelling) {
    std::int64_t n = 1'000'000 + std::int64_t(tokens.size());
    ConstId c = ast::int_value(interners(), target(), ast::Builtin::unsigned_long_long, n);
    tokens[std::to_underlying(c)] = spelling;
    return c;
  }
  ConstId string_address(ConstId s) { return token("&" + tokens.at(std::to_underlying(s))); }
  bool is_constexpr(EntityId e) { return constexpr_fns.contains(std::to_underlying(e)); }
  bool is_main(EntityId e) { return e == main_fn; }
  std::optional<std::uint32_t> ellipsis_after(EntityId e) {
    if (auto it = variadic_fns.find(std::to_underlying(e)); it != variadic_fns.end()) return it->second;
    return std::nullopt;
  }
};

static_assert(Facts<TestFacts>);

// The test's universe: the α slice's types on AArch64 Darwin, and a few entities.
struct World {
  ast::Interners in;
  TestFacts f{in, ast::aarch64_darwin};
  QualType void_t, bool_t, char_t, int_t, unsigned_t, long_t, cchar_t, cchar_p, char5_t;
  QualType fn_ii, fn_iii, fn_i, fn_v, fn_uu, fn_icp, fn_ii_b;
  std::map<std::uint32_t, std::string> names;

  QualType named(QualType q, std::string name) {
    names[std::to_underlying(q)] = std::move(name);
    return q;
  }
  QualType fn(QualType ret, std::initializer_list<QualType> params) {
    return ast::function_type(in, ret, std::span(params.begin(), params.size()));
  }

  World() {
    using enum ast::Builtin;
    void_t = named(ast::builtin(in, void_), "void");
    bool_t = named(ast::builtin(in, bool_), "bool");
    char_t = named(ast::builtin(in, char_), "char");
    int_t = named(ast::builtin(in, int_), "int");
    unsigned_t = named(ast::builtin(in, unsigned_int), "unsigned");
    long_t = named(ast::builtin(in, long_), "long");
    cchar_t = named(ast::builtin(in, char_, ast::Cv::const_), "const_char");
    cchar_p = ast::pointer_to(in, cchar_t);
    char5_t = ast::array_of(in, ast::aarch64_darwin, cchar_t, ast::int_value(in, ast::aarch64_darwin, unsigned_long, 6));
    fn_ii = fn(int_t, {int_t});
    fn_iii = fn(int_t, {int_t, int_t});
    fn_i = fn(int_t, {});
    fn_v = fn(void_t, {});
    fn_uu = fn(unsigned_t, {unsigned_t});
    fn_icp = fn(int_t, {cchar_p});
    fn_ii_b = fn(bool_t, {int_t, int_t});
  }

  std::string type_name(QualType q) {
    if (auto it = names.find(std::to_underlying(q)); it != names.end()) return it->second;
    if (ast::is_pointer(in, q)) return type_name(ast::pointee_of(in, q)) + "*";
    if (ast::is_array(in, q)) {
      auto a = in.types.get<ast::TArray>(in.types.canon(q));
      return std::format("{}[{}]", type_name(a.elem), ast::int_bits(in, a.bound).lo);
    }
    return std::format("T{}", std::to_underlying(q));
  }
  std::string const_name(std::uint32_t n) {
    if (auto it = f.tokens.find(n); it != f.tokens.end()) return it->second;
    ConstId c{n};
    if (in.values.is<ast::VNullptr>(c)) return "null";
    return std::format("{}", std::int64_t(ast::int_bits(in, c).lo));
  }

  // The printed body with handles named: `T<n>` by type name, `@<n>` by entity name and `#<n>` by
  // constant value.
  std::string text(EntityId fn, const ir::Function& body) {
    std::string raw = ir::print(fn, body), out;
    for (std::size_t i = 0; i < raw.size();) {
      char c = raw[i];
      std::size_t j = i + 1;
      if ((c == 'T' || c == '@' || c == '#') && j < raw.size() && std::isdigit(raw[j]) &&
          (i == 0 || !std::isalnum(raw[i - 1]))) {
        while (j < raw.size() && std::isdigit(raw[j])) ++j;
        std::uint32_t n = std::uint32_t(std::stoul(raw.substr(i + 1, j - i - 1)));
        if (c == 'T') out += type_name(QualType(n));
        else if (c == '@') out += "@" + f.entities.at(n);
        else out += "#" + const_name(n);
        i = j;
      } else {
        out += c;
        ++i;
      }
    }
    return out;
  }
};

// ------------------------------------------------------------------ building bodies

// A body under construction: nodes are drafts of one frame builder, committed by `finish`. Child
// offsets are left 0 except where a test sets them.
struct Body {
  explicit Body(World& world) : w(world) {}
  World& w;
  ast::Builder b{w.in};
  std::vector<ast::LocalRec> locals;

  ast::ExprInfo pr(QualType t) { return {t, ast::ValueCategory::prvalue, 0}; }
  ast::ExprInfo lv(QualType t) { return {t, ast::ValueCategory::lvalue, 0}; }
  template <class T>
  NodeId add(const T& v) {
    return b.add(v, 1);
  }

  NodeId lit(QualType t, std::string spelling) {
    std::int64_t v = spelling.starts_with('\'') ? std::int64_t(spelling[1]) : std::stoll(spelling);
    return add(ast::Literal{pr(t), w.f.int_constant(t, v)});
  }
  NodeId str(std::string s) { return add(ast::Literal{lv(w.char5_t), w.f.token("\"" + s + "\"")}); }
  // The parameters are the first locals (#41): declare them before anything else.
  std::map<EntityId, std::uint32_t> param_locals;
  void params(std::initializer_list<std::pair<EntityId, QualType>> ps) {
    for (auto [e, t] : ps) param_locals[e] = declare(w.f.entities.at(std::to_underlying(e)), t);
  }
  NodeId param(EntityId e, QualType) { return local(param_locals.at(e)); }
  NodeId global(EntityId e, QualType t) { return add(ast::DeclRef{lv(t), e}); }
  NodeId local(std::uint32_t i) { return add(ast::LocalRef{lv(locals[i].type), i}); }
  NodeId conv(ast::ConversionKind k, QualType to, NodeId from) {
    return add(ast::ImplicitConversion{pr(to), k, EntityId::none, {from, 0}});
  }
  NodeId read(NodeId lvalue, QualType t) { return conv(ast::ConversionKind::lvalue_to_rvalue, t, lvalue); }
  NodeId read_local(std::uint32_t i) { return read(local(i), locals[i].type); }
  NodeId bin(ast::BinaryOp op, QualType t, NodeId l, NodeId r) { return add(ast::Binary{pr(t), op, {l, 0}, {r, 0}}); }
  NodeId ord(ast::OrderedOp op, QualType t, NodeId l, NodeId r) { return add(ast::OrderedBinary{pr(t), op, {l, 0}, {r, 0}}); }
  NodeId un(ast::UnaryOp op, ast::ExprInfo i, NodeId o) { return add(ast::Unary{i, op, {o, 0}}); }
  NodeId assign(ast::AssignOp op, QualType t, NodeId l, NodeId r) { return add(ast::Assign{lv(t), op, {l, 0}, {r, 0}, t}); }
  NodeId call(EntityId f, QualType t, std::initializer_list<NodeId> args) {
    std::vector<Child> cs;
    for (NodeId a : args) cs.push_back({a, 0});
    NodeId callee = add(ast::DeclRef{lv(t), f});
    return add(ast::Call{pr(t), f, {callee, 0}, b.list(cs)});
  }
  NodeId cond(QualType t, NodeId c, NodeId x, NodeId y) {
    return add(ast::Conditional{pr(t), {c, 0}, {x, 0}, {y, 0}});
  }

  std::uint32_t declare(std::string name, QualType t) {
    locals.push_back({w.in.names.intern(name), t, EntityId::none, NodeId::none, 0});
    return std::uint32_t(locals.size() - 1);
  }
  NodeId var(std::uint32_t i, NodeId init = NodeId::none) {
    NodeId v = add(ast::LocalVar{i, {init, 0}, ast::EntityId::none});
    return add(ast::DeclStmt{b.list({{v, 0}})});
  }
  NodeId ret(NodeId v = NodeId::none) { return add(ast::Return{{v, 0}}); }
  NodeId expr(NodeId e) { return add(ast::ExprStmt{{e, 0}}); }
  NodeId block(std::initializer_list<NodeId> stmts, std::initializer_list<std::uint32_t> offsets = {}) {
    std::vector<Child> cs;
    auto at = offsets.begin();
    for (NodeId s : stmts) cs.push_back({s, at != offsets.end() ? *at++ : 0});
    return add(ast::Compound{b.list(cs)});
  }
  NodeId if_(NodeId c, NodeId t, NodeId e = NodeId::none) { return add(ast::If{{}, {c, 0}, {t, 0}, {e, 0}}); }
  NodeId while_(NodeId c, NodeId body) { return add(ast::While{{c, 0}, {body, 0}}); }
  NodeId do_(NodeId body, NodeId c) { return add(ast::Do{{body, 0}, {c, 0}}); }
  NodeId for_(NodeId init, NodeId c, NodeId incr, NodeId body) {
    return add(ast::For{{init, 0}, {c, 0}, {incr, 0}, {body, 0}});
  }
  NodeId brk() { return add(ast::Break{}); }
  NodeId cont() { return add(ast::Continue{}); }

  ast::BodySema finish(NodeId tree) {
    auto locs = b.seq(std::span<const ast::LocalRec>(locals));
    return b.commit(ast::BodySema{tree, locs, b.seq(std::span<const ast::ExpansionRec>{}), b.seq(std::span<const ast::u32>{}), ast::QualType{}, 0,
                                  b.seq(std::span<const ast::ClosureDef>{})});
  }
};

struct Fn {
  EntityId entity;
  QualType type;
  std::vector<std::pair<EntityId, QualType>> params;
};

ast::DeclSema decl_of(World& w, const Fn& fn) {
  std::vector<ast::ParamSema> ps;
  for (auto [e, t] : fn.params) ps.push_back({e, t, w.in.names.intern(w.f.entities.at(std::to_underlying(e))), NodeId::none, 0});
  auto params = w.in.intern_seq(std::span<const ast::ParamSema>(ps));
  return {fn.entity, fn.type, ast::DeclKind::function, ast::Linkage::external, ast::Storage::none,
          ast::Access::none, 0, {}, NodeId::none, ast::ConstId::none, NodeId::none, params,
          w.in.intern_seq(std::span<const ast::TemplateParamSema>{})};
}

FunctionIr generate(World& w, const Fn& fn, ast::BodySema body, std::uint32_t body_offset = 0,
                    std::source_location at = std::source_location::current()) {
  FunctionInput input{fn.entity, decl_of(w, fn), body, ast::ItemId(7), body_offset};
  auto r = function_ir(w.in, input, w.f);
  if (!r) {
    check(false, r.error(), at);
    return {};
  }
  auto v = ir::verify(r->body);
  check(v.has_value(), v ? "" : v.error() + "\n" + w.text(fn.entity, r->body), at);
  return std::move(*r);
}

// Generates, verifies and compares the named text form with `expected` (leading newline dropped).
void expect(World& w, const Fn& fn, ast::BodySema body, std::string_view expected,
            std::source_location at = std::source_location::current()) {
  FunctionIr r = generate(w, fn, body, 0, at);
  std::string got = w.text(fn.entity, r.body);
  if (expected.starts_with('\n')) expected.remove_prefix(1);
  check(got == expected, std::format("IR differs; got:\n{}", got), at);
  // The text form parses back to the same body (the printer's handles are numbers).
  auto parsed = ir::parse(ir::print(fn.entity, r.body));
  check(parsed && parsed->size() == 1 && parsed->front().body == r.body, "the text form round-trips", at);
}

// ------------------------------------------------------------------ α0

void alpha0() {
  // int main() { return 42; }
  World w;
  Body b{w};
  EntityId main_e = w.f.entity("main");
  w.f.main_fn = main_e;
  auto body = b.finish(b.block({b.ret(b.lit(w.int_t, "42"))}));
  expect(w, {main_e, w.fn_i, {}}, body, R"(
func @main {
^0(%0: !state):
  %1: int = const [#42]
  ret %0, %1
}
)");
}

// ------------------------------------------------------------------ α1

void params_and_if() {
  // int max(int a, int b) { if (a < b) return b; else return a; }
  World w;
  Body b{w};
  EntityId f = w.f.entity("max"), a = w.f.entity("a"), bb = w.f.entity("b");
  b.params({{a, w.int_t}, {bb, w.int_t}});
  auto rd = [&](EntityId e) { return b.read(b.param(e, w.int_t), w.int_t); };
  auto body = b.finish(b.block({b.if_(b.bin(ast::BinaryOp::lt, w.bool_t, rd(a), rd(bb)), b.ret(rd(bb)), b.ret(rd(a)))}));
  expect(w, {f, w.fn_iii, {{a, w.int_t}, {bb, w.int_t}}}, body, R"(
func @max {
^0(%0: !state, %1: int, %2: int):
  %3: !state, %4: int* = alloc %0 [int, automatic, indeterminate]
  %5: !state = store %3, %4, %1 [int, /]
  %6: !state = obj.begin %5, %4 [int]
  %7: !state, %8: int* = alloc %6 [int, automatic, indeterminate]
  %9: !state = store %7, %8, %2 [int, /]
  %10: !state = obj.begin %9, %8 [int]
  %11: int = load %10, %4 [int, /]
  %12: int = load %10, %8 [int, /]
  %13: bool = icmp %11, %12 [slt]
  cond_br %13 ^2(%10), ^1(%10)
^1(%14: !state):
  %15: int = load %14, %4 [int, /]
  %16: !state = obj.end %14, %8
  %17: !state = dealloc %16, %8 [automatic]
  %18: !state = obj.end %17, %4
  %19: !state = dealloc %18, %4 [automatic]
  ret %19, %15
^2(%20: !state):
  %21: int = load %20, %8 [int, /]
  %22: !state = obj.end %20, %8
  %23: !state = dealloc %22, %8 [automatic]
  %24: !state = obj.end %23, %4
  %25: !state = dealloc %24, %4 [automatic]
  ret %25, %21
}
)");
}

void for_loop_unsigned() {
  // unsigned sum(unsigned n) { unsigned s = 0; for (unsigned i = 0; i < n; ++i) s += i; return s; }
  World w;
  Body b{w};
  EntityId f = w.f.entity("sum"), n = w.f.entity("n");
  b.params({{n, w.unsigned_t}});
  auto s = b.declare("s", w.unsigned_t), i = b.declare("i", w.unsigned_t);
  auto body = b.finish(b.block({
      b.var(s, b.lit(w.unsigned_t, "0")),
      b.for_(b.var(i, b.lit(w.unsigned_t, "0")),
             b.bin(ast::BinaryOp::lt, w.bool_t, b.read_local(i), b.read(b.param(n, w.unsigned_t), w.unsigned_t)),
             b.un(ast::UnaryOp::pre_inc, b.lv(w.unsigned_t), b.local(i)),
             b.expr(b.assign(ast::AssignOp::add, w.unsigned_t, b.local(s), b.read_local(i)))),
      b.ret(b.read_local(s)),
  }));
  expect(w, {f, w.fn_uu, {{n, w.unsigned_t}}}, body, R"(
func @sum {
^0(%0: !state, %1: unsigned):
  %2: !state, %3: unsigned* = alloc %0 [unsigned, automatic, indeterminate]
  %4: !state = store %2, %3, %1 [unsigned, /]
  %5: !state = obj.begin %4, %3 [unsigned]
  %6: !state, %7: unsigned* = alloc %5 [unsigned, automatic, indeterminate]
  %8: unsigned = const [#0]
  %9: !state = store %6, %7, %8 [unsigned, /]
  %10: !state = obj.begin %9, %7 [unsigned]
  %11: !state, %12: unsigned* = alloc %10 [unsigned, automatic, indeterminate]
  %13: unsigned = const [#0]
  %14: !state = store %11, %12, %13 [unsigned, /]
  %15: !state = obj.begin %14, %12 [unsigned]
  br ^1(%15)
^1(%16: !state):
  %17: unsigned = load %16, %12 [unsigned, /]
  %18: unsigned = load %16, %3 [unsigned, /]
  %19: bool = icmp %17, %18 [ult]
  cond_br %19 ^3(%16), ^2(%16)
^2(%20: !state):
  %21: !state = obj.end %20, %12
  %22: !state = dealloc %21, %12 [automatic]
  %23: unsigned = load %22, %7 [unsigned, /]
  %24: !state = obj.end %22, %7
  %25: !state = dealloc %24, %7 [automatic]
  %26: !state = obj.end %25, %3
  %27: !state = dealloc %26, %3 [automatic]
  ret %27, %23
^3(%28: !state):
  %29: unsigned = load %28, %7 [unsigned, /]
  %30: unsigned = load %28, %12 [unsigned, /]
  %31: unsigned = add %29, %30
  %32: !state = store %28, %7, %31 [unsigned, /]
  br ^4(%32)
^4(%33: !state):
  %34: unsigned = load %33, %12 [unsigned, /]
  %35: unsigned = const [#1]
  %36: unsigned = add %34, %35
  %37: !state = store %33, %12, %36 [unsigned, /]
  br ^1(%37)
}
)");
}

void while_break_continue() {
  // int f(int n) { int k = 0; while (1) { int t; if (k > n) break; ++k; if (k == 3) continue; } do --n; while (n); return k; }
  World w;
  Body b{w};
  EntityId f = w.f.entity("f"), n = w.f.entity("n");
  b.params({{n, w.int_t}});
  auto k = b.declare("k", w.int_t), t = b.declare("t", w.int_t);
  auto pn = [&] { return b.param(n, w.int_t); };
  auto body = b.finish(b.block({
      b.var(k, b.lit(w.int_t, "0")),
      b.while_(b.conv(ast::ConversionKind::boolean, w.bool_t, b.lit(w.int_t, "1")),
               b.block({b.var(t), b.if_(b.bin(ast::BinaryOp::gt, w.bool_t, b.read_local(k), b.read(pn(), w.int_t)), b.brk()),
                        b.expr(b.un(ast::UnaryOp::pre_inc, b.lv(w.int_t), b.local(k))),
                        b.if_(b.bin(ast::BinaryOp::eq, w.bool_t, b.read_local(k), b.lit(w.int_t, "3")), b.cont())})),
      b.do_(b.expr(b.un(ast::UnaryOp::pre_dec, b.lv(w.int_t), pn())),
            b.conv(ast::ConversionKind::boolean, w.bool_t, b.read(pn(), w.int_t))),
      b.ret(b.read_local(k)),
  }));
  // `t` has no initializer, so it is erroneous ([dcl.init.general]), and it ends its lifetime on
  // each of the three ways out of the loop body: the normal exit, `break` and `continue`.
  expect(w, {f, w.fn_ii, {{n, w.int_t}}}, body, R"(
func @f {
^0(%0: !state, %1: int):
  %2: !state, %3: int* = alloc %0 [int, automatic, indeterminate]
  %4: !state = store %2, %3, %1 [int, /]
  %5: !state = obj.begin %4, %3 [int]
  %6: !state, %7: int* = alloc %5 [int, automatic, indeterminate]
  %8: int = const [#0]
  %9: !state = store %6, %7, %8 [int, /]
  %10: !state = obj.begin %9, %7 [int]
  br ^1(%10)
^1(%11: !state):
  %12: int = const [#1]
  %13: bool = tobool %12
  cond_br %13 ^2(%11), ^7(%11)
^2(%14: !state):
  %15: !state, %16: int* = alloc %14 [int, automatic, erroneous]
  %17: !state = obj.begin %15, %16 [int]
  %18: int = load %17, %7 [int, /]
  %19: int = load %17, %3 [int, /]
  %20: bool = icmp %18, %19 [sgt]
  cond_br %20 ^6(%17), ^3(%17)
^3(%21: !state):
  %22: int = load %21, %7 [int, /]
  %23: int = const [#1]
  %24: int = add nsw %22, %23
  %25: !state = store %21, %7, %24 [int, /]
  %26: int = load %25, %7 [int, /]
  %27: int = const [#3]
  %28: bool = icmp %26, %27 [eq]
  cond_br %28 ^5(%25), ^4(%25)
^4(%29: !state):
  %30: !state = obj.end %29, %16
  %31: !state = dealloc %30, %16 [automatic]
  br ^1(%31)
^5(%32: !state):
  %33: !state = obj.end %32, %16
  %34: !state = dealloc %33, %16 [automatic]
  br ^1(%34)
^6(%35: !state):
  %36: !state = obj.end %35, %16
  %37: !state = dealloc %36, %16 [automatic]
  br ^7(%37)
^7(%38: !state):
  br ^8(%38)
^8(%39: !state):
  %40: int = load %39, %3 [int, /]
  %41: int = const [#1]
  %42: int = sub nsw %40, %41
  %43: !state = store %39, %3, %42 [int, /]
  br ^9(%43)
^9(%44: !state):
  %45: int = load %44, %3 [int, /]
  %46: bool = tobool %45
  cond_br %46 ^8(%44), ^10(%44)
^10(%47: !state):
  %48: int = load %47, %7 [int, /]
  %49: !state = obj.end %47, %7
  %50: !state = dealloc %49, %7 [automatic]
  %51: !state = obj.end %50, %3
  %52: !state = dealloc %51, %3 [automatic]
  ret %52, %48
}
)");
}

void short_circuit_and_conditional() {
  // bool f(int a, int b) { return a && b || (a ? a : b); }  (conversions as sema writes them)
  World w;
  Body b{w};
  EntityId f = w.f.entity("f"), a = w.f.entity("a"), bb = w.f.entity("b");
  b.params({{a, w.int_t}, {bb, w.int_t}});
  auto rd = [&](EntityId e) { return b.read(b.param(e, w.int_t), w.int_t); };
  auto tb = [&](NodeId x) { return b.conv(ast::ConversionKind::boolean, w.bool_t, x); };
  auto both = b.ord(ast::OrderedOp::logical_and, w.bool_t, tb(rd(a)), tb(rd(bb)));
  auto pick = tb(b.cond(w.int_t, tb(rd(a)), rd(a), rd(bb)));
  auto body = b.finish(b.block({b.ret(b.ord(ast::OrderedOp::logical_or, w.bool_t, both, pick))}));
  expect(w, {f, w.fn_ii_b, {{a, w.int_t}, {bb, w.int_t}}}, body, R"(
func @f {
^0(%0: !state, %1: int, %2: int):
  %3: !state, %4: int* = alloc %0 [int, automatic, indeterminate]
  %5: !state = store %3, %4, %1 [int, /]
  %6: !state = obj.begin %5, %4 [int]
  %7: !state, %8: int* = alloc %6 [int, automatic, indeterminate]
  %9: !state = store %7, %8, %2 [int, /]
  %10: !state = obj.begin %9, %8 [int]
  %11: int = load %10, %4 [int, /]
  %12: bool = tobool %11
  cond_br %12 ^1(%10), ^2(%10, %12)
^1(%13: !state):
  %14: int = load %13, %8 [int, /]
  %15: bool = tobool %14
  br ^2(%13, %15)
^2(%16: !state, %17: bool):
  cond_br %17 ^7(%16, %17), ^3(%16)
^3(%18: !state):
  %19: int = load %18, %4 [int, /]
  %20: bool = tobool %19
  cond_br %20 ^5(%18), ^4(%18)
^4(%21: !state):
  %22: int = load %21, %8 [int, /]
  br ^6(%21, %22)
^5(%23: !state):
  %24: int = load %23, %4 [int, /]
  br ^6(%23, %24)
^6(%25: !state, %26: int):
  %27: bool = tobool %26
  br ^7(%25, %27)
^7(%28: !state, %29: bool):
  %30: !state = obj.end %28, %8
  %31: !state = dealloc %30, %8 [automatic]
  %32: !state = obj.end %31, %4
  %33: !state = dealloc %32, %4 [automatic]
  ret %33, %29
}
)");
}

void recursion_and_promotions() {
  // constexpr int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
  {
    World w;
    Body b{w};
    EntityId f = w.f.entity("fact"), n = w.f.entity("n");
    b.params({{n, w.int_t}});
    w.f.constexpr_fns.insert(std::to_underlying(f));
    auto rn = [&] { return b.read(b.param(n, w.int_t), w.int_t); };
    auto body = b.finish(b.block({b.ret(b.cond(
        w.int_t, b.bin(ast::BinaryOp::le, w.bool_t, rn(), b.lit(w.int_t, "1")), b.lit(w.int_t, "1"),
        b.bin(ast::BinaryOp::mul, w.int_t, rn(), b.call(f, w.int_t, {b.bin(ast::BinaryOp::sub, w.int_t, rn(), b.lit(w.int_t, "1"))}))))}));
    FunctionIr r = generate(w, {f, w.fn_ii, {{n, w.int_t}}}, body);
    std::string got = w.text(f, r.body);
    check(got.contains("= call %") && got.contains("[@fact, 0, 0, 0]") && !got.contains("not_constexpr"),
          "a call to a constexpr function is not flagged:\n" + got);
    check(got.contains("mul nsw") && got.contains("sub nsw"), "signed arithmetic is nsw:\n" + got);
    // `n * fact(n - 1)`: what `fact` does is indeterminately sequenced with the read of n, not
    // unsequenced ([intro.execution]), so there is no useq group.
    check(!got.contains("useq"), "no useq group around a call:\n" + got);
  }
  {
    // int g() { char c = 'a'; ++c; return c + 1; }: `++c` is done in int and wraps back to char.
    World w;
    Body b{w};
    EntityId g = w.f.entity("g");
    auto c = b.declare("c", w.char_t);
    auto body = b.finish(b.block({
        b.var(c, b.lit(w.char_t, "'a'")),
        b.expr(b.un(ast::UnaryOp::pre_inc, b.lv(w.char_t), b.local(c))),
        b.ret(b.bin(ast::BinaryOp::add, w.int_t, b.conv(ast::ConversionKind::integral, w.int_t, b.read_local(c)), b.lit(w.int_t, "1"))),
    }));
    expect(w, {g, w.fn_i, {}}, body, R"(
func @g {
^0(%0: !state):
  %1: !state, %2: char* = alloc %0 [char, automatic, indeterminate]
  %3: char = const [#97]
  %4: !state = store %1, %2, %3 [char, /]
  %5: !state = obj.begin %4, %2 [char]
  %6: char = load %5, %2 [char, /]
  %7: int = iconv %6
  %8: int = const [#1]
  %9: int = add nsw %7, %8
  %10: char = iconv %9
  %11: !state = store %5, %2, %10 [char, /]
  %12: char = load %11, %2 [char, /]
  %13: !state = obj.end %11, %2
  %14: !state = dealloc %13, %2 [automatic]
  %15: int = iconv %12
  %16: int = const [#1]
  %17: int = add nsw %15, %16
  ret %14, %17
}
)");
  }
}

// ------------------------------------------------------------------ α2

void hello_world() {
  // extern "C" int puts(const char*); int main() { puts("hello"); }
  World w;
  Body b{w};
  EntityId main_e = w.f.entity("main"), puts_e = w.f.entity("puts");
  w.f.main_fn = main_e;
  auto arg = b.conv(ast::ConversionKind::array_to_pointer, w.cchar_p, b.str("hello"));
  auto body = b.finish(b.block({b.expr(b.call(puts_e, w.int_t, {arg}))}));
  expect(w, {main_e, w.fn_i, {}}, body, R"(
func @main {
^0(%0: !state):
  %1: const_char[6]* = const [#&"hello"]
  %2: const_char* = ptr_member %1 [char, /e0]
  %3: !state, %4: int = call not_constexpr %0, %2 [@puts, 0, 0, 0]
  %5: int = const [#0]
  ret %3, %5
}
)");
}

void ellipsis() {
  // extern "C" int printf(const char*, ...); struct S { int m(int, ...); };
  // int main() { printf("hi", 1, 2L); p->m(3, 4); }  (the object here is a null pointer)
  // The call records how many arguments the ellipsis matches; a member call's object is not one.
  World w;
  Body b{w};
  EntityId main_e = w.f.entity("main"), printf_e = w.f.entity("printf"), m_e = w.f.entity("m");
  w.f.main_fn = main_e;
  w.f.variadic_fns[std::to_underlying(printf_e)] = 1;
  w.f.variadic_fns[std::to_underlying(m_e)] = 1;
  auto fmt = b.conv(ast::ConversionKind::array_to_pointer, w.cchar_p, b.str("hi"));
  auto object = b.conv(ast::ConversionKind::null_pointer, w.cchar_p, b.lit(w.int_t, "0"));
  std::vector<Child> margs{{b.lit(w.int_t, "3"), 0}, {b.lit(w.int_t, "4"), 0}};
  auto member = b.add(ast::MemberCall{b.pr(w.int_t), m_e, {object, 0}, b.b.list(margs)});
  auto body = b.finish(b.block({b.expr(b.call(printf_e, w.int_t, {fmt, b.lit(w.int_t, "1"), b.lit(w.long_t, "2")})),
                                b.expr(member)}));
  expect(w, {main_e, w.fn_i, {}}, body, R"(
func @main {
^0(%0: !state):
  %1: const_char[6]* = const [#&"hi"]
  %2: const_char* = ptr_member %1 [char, /e0]
  %3: int = const [#1]
  %4: long = const [#2]
  %5: !state, %6: int = call not_constexpr %0, %2, %3, %4 [@printf, 2, 0, 0]
  %7: const_char* = const [#null]
  %8: int = const [#3]
  %9: int = const [#4]
  %10: !state, %11: int = call not_constexpr %5, %7, %8, %9 [@m, 1, 0, 0]
  %12: int = const [#0]
  ret %10, %12
}
)");
}

void globals_and_pointers() {
  // int counter = 3;  int next() { counter++; return counter; }
  {
    World w;
    Body b{w};
    EntityId f = w.f.entity("next"), counter = w.f.entity("counter");
    auto body = b.finish(b.block({
        b.expr(b.un(ast::UnaryOp::post_inc, b.pr(w.int_t), b.global(counter, w.int_t))),
        b.ret(b.read(b.global(counter, w.int_t), w.int_t)),
    }));
    expect(w, {f, w.fn_i, {}}, body, R"(
func @next {
^0(%0: !state):
  %1: int* = global [@counter]
  %2: int = load %0, %1 [int, /]
  %3: int = const [#1]
  %4: int = add nsw %2, %3
  %5: !state = store %0, %1, %4 [int, /]
  %6: int* = global [@counter]
  %7: int = load %5, %6 [int, /]
  ret %5, %7
}
)");
  }
  {
    // int len(const char* s) { int n = 0; while (*s++) n += 1; return n; }  — with s[0] too.
    World w;
    Body b{w};
    EntityId f = w.f.entity("len"), s = w.f.entity("s");
    b.params({{s, w.cchar_p}});
    auto n = b.declare("n", w.int_t);
    auto ps = [&] { return b.param(s, w.cchar_p); };
    auto deref_post = b.un(ast::UnaryOp::deref, b.lv(w.cchar_t), b.un(ast::UnaryOp::post_inc, b.pr(w.cchar_p), ps()));
    auto first = b.add(ast::Subscript{b.lv(w.cchar_t), {b.read(ps(), w.cchar_p), 0}, {b.lit(w.int_t, "0"), 0}});
    auto body = b.finish(b.block({
        b.var(n, b.conv(ast::ConversionKind::integral, w.int_t, b.read(first, w.cchar_t))),
        b.while_(b.conv(ast::ConversionKind::boolean, w.bool_t, b.read(deref_post, w.cchar_t)),
                 b.expr(b.assign(ast::AssignOp::add, w.int_t, b.local(n), b.lit(w.int_t, "1")))),
        b.ret(b.read_local(n)),
    }));
    // `*s++` reads through the old pointer (%16), and stores the incremented one.
    expect(w, {f, w.fn_icp, {{s, w.cchar_p}}}, body, R"(
func @len {
^0(%0: !state, %1: const_char*):
  %2: !state, %3: const_char** = alloc %0 [const_char*, automatic, indeterminate]
  %4: !state = store %2, %3, %1 [const_char*, /]
  %5: !state = obj.begin %4, %3 [const_char*]
  %6: !state, %7: int* = alloc %5 [int, automatic, indeterminate]
  %8: const_char* = load %6, %3 [const_char*, /]
  %9: int = const [#0]
  %10: const_char* = ptr_add %8, %9 [char]
  %11: char = load %6, %10 [char, /]
  %12: int = iconv %11
  %13: !state = store %6, %7, %12 [int, /]
  %14: !state = obj.begin %13, %7 [int]
  br ^1(%14)
^1(%15: !state):
  %16: const_char* = load %15, %3 [const_char*, /]
  %17: int = const [#1]
  %18: const_char* = ptr_add %16, %17 [char]
  %19: !state = store %15, %3, %18 [const_char*, /]
  %20: char = load %19, %16 [char, /]
  %21: bool = tobool %20
  cond_br %21 ^3(%19), ^2(%19)
^2(%22: !state):
  %23: int = load %22, %7 [int, /]
  %24: !state = obj.end %22, %7
  %25: !state = dealloc %24, %7 [automatic]
  %26: !state = obj.end %25, %3
  %27: !state = dealloc %26, %3 [automatic]
  ret %27, %23
^3(%28: !state):
  %29: int = load %28, %7 [int, /]
  %30: int = const [#1]
  %31: int = add nsw %29, %30
  %32: !state = store %28, %7, %31 [int, /]
  br ^1(%32)
}
)");
  }
}

void unsequenced() {
  // int u(int x) { return x + (x = 1); }: the read and the write of x are unsequenced, so they
  // are a useq group, and the interpreter reports the conflict.
  World w;
  Body b{w};
  EntityId u = w.f.entity("u"), x = w.f.entity("x");
  b.params({{x, w.int_t}});
  auto px = [&] { return b.param(x, w.int_t); };
  auto body = b.finish(b.block({b.ret(b.bin(ast::BinaryOp::add, w.int_t, b.read(px(), w.int_t),
                                            b.read(b.assign(ast::AssignOp::assign, w.int_t, px(), b.lit(w.int_t, "1")), w.int_t)))}));
  expect(w, {u, w.fn_ii, {{x, w.int_t}}}, body, R"(
func @u {
^0(%0: !state, %1: int):
  %2: !state, %3: int* = alloc %0 [int, automatic, indeterminate]
  %4: !state = store %2, %3, %1 [int, /]
  %5: !state = obj.begin %4, %3 [int]
  %6: !state = useq.begin %5
  %7: int = load %6, %3 [int, /]
  %8: !state = useq.next %6
  %9: int = const [#1]
  %10: !state = store %8, %3, %9 [int, /]
  %11: int = load %10, %3 [int, /]
  %12: !state = useq.end %10
  %13: !state = obj.end %12, %3
  %14: !state = dealloc %13, %3 [automatic]
  %15: int = add nsw %7, %11
  ret %14, %15
}
)");
}

void division_shifts_and_pointers() {
  // int d(int a, unsigned u, const char* p, char c) {
  //   c += 1000;                       // computed in int, narrowed back to char
  //   a = a / 3; a = a % 3; a = a >> 1;
  //   u = u / 3u; u = u % 3u; u = u >> 1;
  //   p = p - u; p -= u;               // an unsigned index is negated in ptrdiff_t
  //   return *p;
  // }
  World w;
  Body b{w};
  EntityId d = w.f.entity("d"), a = w.f.entity("a"), u = w.f.entity("u"), p = w.f.entity("p"), c = w.f.entity("c");
  b.params({{a, w.int_t}, {u, w.unsigned_t}, {p, w.cchar_p}, {c, w.char_t}});
  auto ref = [&](EntityId e, QualType t) { return b.param(e, t); };
  auto rd = [&](EntityId e, QualType t) { return b.read(ref(e, t), t); };
  auto set = [&](EntityId e, QualType t, NodeId v) { return b.expr(b.assign(ast::AssignOp::assign, t, ref(e, t), v)); };
  using enum ast::BinaryOp;
  auto c_add = b.add(ast::Assign{b.lv(w.char_t), ast::AssignOp::add, {ref(c, w.char_t), 0}, {b.lit(w.int_t, "1000"), 0}, w.int_t});
  auto p_sub = b.add(ast::Assign{b.lv(w.cchar_p), ast::AssignOp::sub, {ref(p, w.cchar_p), 0}, {rd(u, w.unsigned_t), 0}, w.cchar_p});
  QualType fn = w.fn(w.int_t, {w.int_t, w.unsigned_t, w.cchar_p, w.char_t});
  auto body = b.finish(b.block({
      b.expr(c_add),
      set(a, w.int_t, b.bin(div, w.int_t, rd(a, w.int_t), b.lit(w.int_t, "3"))),
      set(a, w.int_t, b.bin(rem, w.int_t, rd(a, w.int_t), b.lit(w.int_t, "3"))),
      set(a, w.int_t, b.ord(ast::OrderedOp::shr, w.int_t, rd(a, w.int_t), b.lit(w.int_t, "1"))),
      set(u, w.unsigned_t, b.bin(div, w.unsigned_t, rd(u, w.unsigned_t), b.lit(w.unsigned_t, "3"))),
      set(u, w.unsigned_t, b.bin(rem, w.unsigned_t, rd(u, w.unsigned_t), b.lit(w.unsigned_t, "3"))),
      set(u, w.unsigned_t, b.ord(ast::OrderedOp::shr, w.unsigned_t, rd(u, w.unsigned_t), b.lit(w.int_t, "1"))),
      set(p, w.cchar_p, b.bin(sub, w.cchar_p, rd(p, w.cchar_p), rd(u, w.unsigned_t))),
      b.expr(p_sub),
      b.ret(b.conv(ast::ConversionKind::integral, w.int_t,
                   b.read(b.un(ast::UnaryOp::deref, b.lv(w.cchar_t), rd(p, w.cchar_p)), w.cchar_t))),
  }));
  expect(w, {d, fn, {{a, w.int_t}, {u, w.unsigned_t}, {p, w.cchar_p}, {c, w.char_t}}}, body, R"(
func @d {
^0(%0: !state, %1: int, %2: unsigned, %3: const_char*, %4: char):
  %5: !state, %6: int* = alloc %0 [int, automatic, indeterminate]
  %7: !state = store %5, %6, %1 [int, /]
  %8: !state = obj.begin %7, %6 [int]
  %9: !state, %10: unsigned* = alloc %8 [unsigned, automatic, indeterminate]
  %11: !state = store %9, %10, %2 [unsigned, /]
  %12: !state = obj.begin %11, %10 [unsigned]
  %13: !state, %14: const_char** = alloc %12 [const_char*, automatic, indeterminate]
  %15: !state = store %13, %14, %3 [const_char*, /]
  %16: !state = obj.begin %15, %14 [const_char*]
  %17: !state, %18: char* = alloc %16 [char, automatic, indeterminate]
  %19: !state = store %17, %18, %4 [char, /]
  %20: !state = obj.begin %19, %18 [char]
  %21: char = load %20, %18 [char, /]
  %22: int = iconv %21
  %23: int = const [#1000]
  %24: int = add nsw %22, %23
  %25: char = iconv %24
  %26: !state = store %20, %18, %25 [char, /]
  %27: int = load %26, %6 [int, /]
  %28: int = const [#3]
  %29: int = sdiv %27, %28
  %30: !state = store %26, %6, %29 [int, /]
  %31: int = load %30, %6 [int, /]
  %32: int = const [#3]
  %33: int = srem %31, %32
  %34: !state = store %30, %6, %33 [int, /]
  %35: int = load %34, %6 [int, /]
  %36: int = const [#1]
  %37: int = ashr %35, %36
  %38: !state = store %34, %6, %37 [int, /]
  %39: unsigned = load %38, %10 [unsigned, /]
  %40: unsigned = const [#3]
  %41: unsigned = udiv %39, %40
  %42: !state = store %38, %10, %41 [unsigned, /]
  %43: unsigned = load %42, %10 [unsigned, /]
  %44: unsigned = const [#3]
  %45: unsigned = urem %43, %44
  %46: !state = store %42, %10, %45 [unsigned, /]
  %47: unsigned = load %46, %10 [unsigned, /]
  %48: int = const [#1]
  %49: unsigned = lshr %47, %48
  %50: !state = store %46, %10, %49 [unsigned, /]
  %51: const_char* = load %50, %14 [const_char*, /]
  %52: unsigned = load %50, %10 [unsigned, /]
  %53: long = iconv %52
  %54: long = neg %53
  %55: const_char* = ptr_add %51, %54 [char]
  %56: !state = store %50, %14, %55 [const_char*, /]
  %57: const_char* = load %56, %14 [const_char*, /]
  %58: unsigned = load %56, %10 [unsigned, /]
  %59: long = iconv %58
  %60: long = neg %59
  %61: const_char* = ptr_add %57, %60 [char]
  %62: !state = store %56, %14, %61 [const_char*, /]
  %63: const_char* = load %62, %14 [const_char*, /]
  %64: char = load %62, %63 [char, /]
  %65: !state = obj.end %62, %18
  %66: !state = dealloc %65, %18 [automatic]
  %67: !state = obj.end %66, %14
  %68: !state = dealloc %67, %14 [automatic]
  %69: !state = obj.end %68, %10
  %70: !state = dealloc %69, %10 [automatic]
  %71: !state = obj.end %70, %6
  %72: !state = dealloc %71, %6 [automatic]
  %73: int = iconv %64
  ret %72, %73
}
)");
}

void void_return() {
  // void g(); void h() { return g(); }: [stmt.return] allows a void operand.
  World w;
  Body b{w};
  EntityId g = w.f.entity("g"), h = w.f.entity("h");
  auto body = b.finish(b.block({b.ret(b.call(g, w.void_t, {}))}));
  expect(w, {h, w.fn_v, {}}, body, R"(
func @h {
^0(%0: !state):
  %1: !state = call not_constexpr %0 [@g, 0, 0, 0]
  ret %1
}
)");
}

void more_expressions() {
  // int h(int a, const char* p) {
  //   int x = 1, y = 2;
  //   (a ? x : y) = 3;          // a glvalue conditional
  //   x <<= a; y >>= 1;         // compound shifts
  //   if (!(p == nullptr)) x = ~x;
  //   if (a) return x; else return y;
  //   x = 4;                    // unreachable
  // }
  World w;
  Body b{w};
  EntityId h = w.f.entity("h"), a = w.f.entity("a"), p = w.f.entity("p");
  b.params({{a, w.int_t}, {p, w.cchar_p}});
  auto x = b.declare("x", w.int_t), y = b.declare("y", w.int_t);
  auto ra = [&] { return b.read(b.param(a, w.int_t), w.int_t); };
  auto tb = [&](NodeId v) { return b.conv(ast::ConversionKind::boolean, w.bool_t, v); };
  auto glc = b.add(ast::Conditional{b.lv(w.int_t), {tb(ra()), 0}, {b.local(x), 0}, {b.local(y), 0}});
  auto null = b.conv(ast::ConversionKind::null_pointer, w.cchar_p, b.lit(w.int_t, "0"));
  auto is_null = b.bin(ast::BinaryOp::eq, w.bool_t, b.read(b.param(p, w.cchar_p), w.cchar_p), null);
  auto body = b.finish(b.block({
      b.var(x, b.lit(w.int_t, "1")),
      b.var(y, b.lit(w.int_t, "2")),
      b.expr(b.assign(ast::AssignOp::assign, w.int_t, glc, b.lit(w.int_t, "3"))),
      b.expr(b.assign(ast::AssignOp::shl, w.int_t, b.local(x), ra())),
      b.expr(b.assign(ast::AssignOp::shr, w.int_t, b.local(y), b.lit(w.int_t, "1"))),
      b.if_(b.un(ast::UnaryOp::not_, b.pr(w.bool_t), is_null),
            b.expr(b.assign(ast::AssignOp::assign, w.int_t, b.local(x), b.un(ast::UnaryOp::compl_, b.pr(w.int_t), b.read_local(x))))),
      b.if_(tb(ra()), b.ret(b.read_local(x)), b.ret(b.read_local(y))),
      b.expr(b.assign(ast::AssignOp::assign, w.int_t, b.local(x), b.lit(w.int_t, "4"))),
  }));
  FunctionIr r = generate(w, {h, w.fn_iii, {{a, w.int_t}, {p, w.cchar_p}}}, body);
  std::string got = w.text(h, r.body);
  check(got.contains("int*) = ") || got.contains(": int*)"), "the glvalue conditional joins on a pointer:\n" + got);
  check(got.contains("shl %") && got.contains("ashr %") && !got.contains("shl nsw"), "shifts:\n" + got);
  check(got.contains("ptr_cmp") && got.contains("[#null]") && !got.contains("[#0]"), "pointer comparison with a null constant:\n" + got);
  check(got.contains("[#-1]") && got.contains("xor"), "~ and ! are xor:\n" + got);
  check(!got.contains("#4"), "code after both returns is dropped:\n" + got);
  check(!got.contains("unreachable"), "no fall-off end is reachable:\n" + got);
}

// ------------------------------------------------------------------ debug, identity, limits

void debug_and_fingerprint() {
  // The same body at two offsets: one fingerprint, different locations.
  World w;
  EntityId f = w.f.entity("f");
  auto make = [&](std::uint32_t at) {
    Body b{w};
    auto x = b.declare("x", w.int_t);
    auto body = b.finish(b.block({b.var(x, b.lit(w.int_t, "1")), b.ret(b.read_local(x))}, {at, at + 5}));
    return body;
  };
  auto b1 = make(1), b2 = make(3);
  FunctionIr r1 = generate(w, {f, w.fn_i, {}}, b1, 10), r2 = generate(w, {f, w.fn_i, {}}, b2, 20);
  check(r1.body == r2.body && ir::fingerprint(r1.body) == ir::fingerprint(r2.body), "positions are not in the body");
  check(r1.debug.loc.size() == r1.body.nodes.size(), "a location per node");
  // Node 0 is x's alloc, at the declaration (body offset 10 + 1); the ret is at the return (+6).
  check(r1.debug.loc[0] == ir::Loc{ast::ItemId(7), 11} && r2.debug.loc[0] == ir::Loc{ast::ItemId(7), 23}, "alloc located at the declaration");
  ir::NodeId last{std::uint32_t(r1.body.nodes.size() - 1)};
  check(r1.body.node(last).op == ir::Op::ret && r1.debug.loc[ir::raw(last)] == ir::Loc{ast::ItemId(7), 16}, "ret located at the return");
  check(r1.debug.vars.size() == 1 && r1.debug.vars[0].var == ir::LocalId(0) && r1.debug.vars[0].nodes.size() == 1 &&
            r1.body.node(r1.debug.vars[0].nodes[0]).op == ir::Op::alloc,
        "x is held by its alloc");
  check(r1.debug.loc != r2.debug.loc, "locations differ");
}

void unsupported() {
  World w;
  Body b{w};
  EntityId f = w.f.entity("f");
  auto body = b.finish(b.block({b.ret(b.bin(ast::BinaryOp::spaceship, w.int_t, b.lit(w.int_t, "1"), b.lit(w.int_t, "2")))}));
  FunctionInput input{f, decl_of(w, {f, w.fn_i, {}}), body, ast::ItemId(7), 0};
  auto r = function_ir(w.in, input, w.f);
  check(!r && r.error().contains("Binary <=>"), r ? "<=> generated" : r.error());
}

}  // namespace

int main() {
  alpha0();
  params_and_if();
  for_loop_unsigned();
  while_break_continue();
  short_circuit_and_conditional();
  recursion_and_promotions();
  hello_world();
  ellipsis();
  globals_and_pointers();
  more_expressions();
  unsequenced();
  division_shifts_and_pointers();
  void_return();
  debug_and_fingerprint();
  unsupported();
  return ycxx::unit::result("irgen");
}
