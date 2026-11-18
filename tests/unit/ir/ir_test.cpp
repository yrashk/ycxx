// Unit tests of the L1 IR (#23): the op table, building and canonical order, the verifier, the
// fingerprint, debug information, the evaluator hook, and the text form's round trip.
import std;
import ycxx.ir;
import ycxx.unit;

using namespace ycxx::ir;
using ycxx::unit::check;

namespace {

NamedFunction parse_one(std::string_view text, std::source_location at = std::source_location::current()) {
  auto m = parse(text);
  if (!m) {
    check(false, std::format("parse error {}:{}: {}", m.error().line, m.error().column, m.error().message), at);
    return {};
  }
  check(m->size() == 1, "one function", at);
  return m->empty() ? NamedFunction{} : std::move(m->front());
}

// Parses, prints, parses the print and prints again: both bodies and both prints are equal.
void round_trip(std::string_view text, std::source_location at = std::source_location::current()) {
  auto f = parse_one(text, at);
  std::string printed = print(f.name, f.body);
  auto g = parse_one(printed, at);
  check(f.body == g.body, std::format("round trip keeps the body:\n{}", printed), at);
  check(print(g.name, g.body) == printed, "printing is a fixed point", at);
  check(fingerprint(f.body) == fingerprint(g.body), "round trip keeps the fingerprint", at);
}

std::string parse_error(std::string_view text) {
  auto m = parse(text);
  return m ? std::string() : m.error().message;
}

// --- The op table -----------------------------------------------------------------------------

static_assert(op_count > 80);
static_assert(info(Op::obj__begin).name == "obj.begin");
static_assert(info(Op::const_).name == "const");
static_assert(info(Op::sdiv).spec.cls == NodeClass::pinned);
static_assert(info(Op::store).spec.cls == NodeClass::state);
static_assert(is_terminator(Op::consteval_br) && !is_terminator(Op::call));
static_assert(spec_of(Op::add).has(Trait::commutative) && !spec_of(Op::sub).has(Trait::commutative));
static_assert(spec_of(Op::ptr_to_int).has(Trait::nonconstant));
static_assert(spec_of(Op::add).flags == (bit(Flag::nsw) | bit(Flag::nuw)));
static_assert(name_of(Flag::volatile_) == "volatile" && name_of(Rmw::and_) == "and");
static_assert(named<Init>("erroneous") == Init::erroneous);

void table_checks() {
  std::set<std::string_view> names;
  for (std::size_t i = 0; i < op_count; ++i) {
    names.insert(op_table[i].name);
    check(op_named(op_table[i].name) == Op(i), std::format("op_named({})", op_table[i].name));
    const OpSpec& s = op_table[i].spec;
    for (std::size_t k = 0; k + 1 < s.in.size; ++k) check(s.in.at[k] != Arg::values, "`values` is last");
    check(s.cls == NodeClass::control || (s.succ == 0 && !s.var_succ), "only terminators have successors");
  }
  check(names.size() == op_count, "op names are unique");
}

// --- Every op, built from its spec, verifies and round-trips ----------------------------------

std::vector<std::uint64_t> sample_imms(Op op, std::uint32_t nsucc) {
  std::vector<std::uint64_t> w;
  for (Imm k : spec_of(op).imm.view()) {
    switch (k) {
      case Imm::type: w.push_back(5); break;
      case Imm::entity: w.push_back(6); break;
      case Imm::constant: w.push_back(7); break;
      case Imm::u32: w.push_back(1); break;
      case Imm::path:
        w.push_back(2);
        w.push_back(encode({Step::field, 1}));
        w.push_back(encode({Step::elem, 3}));
        break;
      case Imm::pred: w.push_back(std::to_underlying(Pred::sle)); break;
      case Imm::storage: w.push_back(std::to_underlying(Storage::temporary)); break;
      case Imm::init: w.push_back(std::to_underlying(Init::erroneous)); break;
      case Imm::order: w.push_back(std::to_underlying(MemOrder::acq_rel)); break;
      case Imm::rmw: w.push_back(std::to_underlying(Rmw::xor_)); break;
      case Imm::consts:
        w.push_back(nsucc - 1);
        for (std::uint32_t i = 1; i < nsucc; ++i) w.push_back(10 + i);
        break;
    }
  }
  return w;
}

Ty sample_type(Arg a) {
  switch (a) {
    case Arg::state: return Ty::state();
    case Arg::exn: return Ty::exn();
    case Arg::coro: return Ty::coro();
    default: return Ty::of(QualType(2));
  }
}

void every_op() {
  for (std::size_t i = 0; i < op_count; ++i) {
    Op op = Op(i);
    const OpSpec& s = spec_of(op);
    Builder b;
    BlockId entry = b.add_block({Ty::state(), Ty::of(QualType(1)), Ty::exn(), Ty::coro()});
    std::vector<ValueId> operands;
    for (Arg a : s.in.view()) {
      switch (a) {
        case Arg::state: operands.push_back(b.param(entry, 0)); break;
        case Arg::value: operands.push_back(b.param(entry, 1)); break;
        case Arg::exn: operands.push_back(b.param(entry, 2)); break;
        case Arg::coro: operands.push_back(b.param(entry, 3)); break;
        case Arg::values: operands.push_back(b.param(entry, 1)); break;
      }
    }
    std::vector<Ty> results;
    for (Arg a : s.out.view()) results.push_back(sample_type(a));
    std::uint32_t nsucc = s.succ + (s.var_succ ? (op == Op::switch_ ? 2 : 1) : 0);
    std::vector<SuccRef> succs;
    for (std::uint32_t k = 0; k < nsucc; ++k) {
      std::vector<Ty> params;
      for (TyKind t : implicit_params(op, k)) params.push_back(Ty{t});
      BlockId t = b.add_block(std::span<const Ty>(params));
      b.emit(t, Op::unreachable, {}, {});
      succs.push_back({t, {}});
    }
    Flags flags = s.flags;  // every flag it accepts
    auto imms = sample_imms(op, nsucc);
    NodeId n = b.emit(entry, op, flags, results, operands, imms, succs);
    if (!is_terminator(op)) {
      ValueId st = b.param(entry, 0);
      if (!s.out.view().empty() && s.out.view()[0] == Arg::state) st = b.result(n, 0);
      b.emit(entry, Op::ret, {}, {st});
    }
    auto built = b.finish();
    std::string_view name = info(op).name;
    check(built.has_value(), std::format("{}: finish: {}", name, built ? "" : built.error()));
    if (!built) continue;
    auto v = verify(built->function);
    check(v.has_value(), std::format("{}: verify: {}", name, v ? "" : v.error()));
    std::string text = print(EntityId(1), built->function);
    auto back = parse(text);
    check(back.has_value(), std::format("{}: parse: {}\n{}", name, back ? "" : back.error().message, text));
    if (!back) continue;
    check((*back)[0].body == built->function, std::format("{}: round trip\n{}", name, text));
  }
}

// --- Hand-written bodies ----------------------------------------------------------------------

constexpr std::string_view loop = R"(
// sum of 0..n-1: a loop through block arguments
func @1 {
^entry(%s: !state, %n: T1):
  %zero: T1 = const [#0]
  br ^head(%s, %zero, %zero)
^head(%s1: !state, %i: T1, %acc: T1):
  %more: T2 = icmp %i, %n [slt]
  cond_br %more ^body, ^exit(%s1, %acc)
^body:
  %one: T1 = const [#1]
  %acc2: T1 = add nsw %acc, %i
  %i2: T1 = add nsw %i, %one
  br ^head(%s1, %i2, %acc2)
^exit(%s2: !state, %r: T1):
  ret %s2, %r
}
)";

constexpr std::string_view objects = R"(
func @2 {
^0(%s: !state, %v: T1):
  %s1: !state, %p: T4 = alloc %s [T4, automatic, erroneous]
  %s2: !state = obj.begin %s1, %p [T4]
  %s3: !state = union.activate %s2, %p [/f1]
  %s4: !state = store volatile %s3, %p, %v [T1, /f1]
  %x: T1 = load %s4, %p [T1, /f1/b0/e2]
  %q: T5 = ptr_member %p [T5, /]
  %d: T6 = ptr_diff %q, %q [T1]
  %s5: !state = useq.begin %s4
  %s6: !state = useq.next %s5
  %s7: !state = useq.end %s6
  %s8: !state = obj.end %s7, %p
  %c: T2 = is_const_eval
  consteval_br ^1(%s8, %x), ^2(%s8)
^1(%t: !state, %y: T1):
  ret %t, %y
^2(%u: !state):
  %z: T1 = const [#3]
  switch %z [(#1, #2)] ^2(%u), ^1(%u, %z), ^1(%u, %x)
}
)";

constexpr std::string_view exceptions = R"(
func @3 {
^0(%s: !state, %a: T1):
  %s1: !state, %r: T1 = invoke %s, %a [@9] ^ok, ^lpad(%a)
^ok:
  ret %s1, %r
^lpad(%t: !state, %e: !exn, %a2: T1):
  %m: T2 = eh.match %e [T7]
  cond_br %m ^catch(%t, %e), ^out(%t, %e)
^catch(%t1: !state, %e1: !exn):
  %t2: !state, %obj: T8 = catch.begin %t1, %e1
  %t3: !state = catch.end %t2, %e1
  ret %t3, %a2
^out(%t4: !state, %e2: !exn):
  resume %t4, %e2
}
)";

constexpr std::string_view coroutine = R"(
func @4 {
^0(%s: !state):
  %s1: !state, %h: !coro = coro.begin %s
  %p: T3 = coro.promise %h
  %s2: !state = call %s1, %p [@20]
  coro.suspend %s2, %h [0] ^resumed, ^destroyed
^resumed(%s3: !state):
  coro.suspend %s3, %h [1] ^destroyed, ^destroyed
^destroyed(%s4: !state):
  coro.end %s4
}
)";

void hand_written() {
  for (auto text : {loop, objects, exceptions, coroutine}) round_trip(text);
  auto f = parse_one(loop);
  check(f.body.blocks.size() == 4 && f.body.block(BlockId(0)).nparams == 2, "loop shape");
  // Canonical block order is reverse post-order: entry, head, then exit before body, since the
  // walk visits `body` first and so finishes it last.
  std::string printed = print(f.name, f.body);
  check(printed.find("^1(%3: !state, %4: T1, %5: T1):") != std::string::npos, "head is block 1:\n" + printed);
}

// --- Canonical order and the fingerprint ------------------------------------------------------

constexpr std::string_view order_a = R"(
func @5 {
^0(%s: !state, %x: T1, %y: T1):
  %a: T1 = add %x, %y
  %b: T1 = mul %a, %x
  %c: T1 = sub %x, %y
  %s1: !state = store %s, %x, %b [T1, /]
  %s2: !state = store %s1, %x, %c [T1, /]
  br ^1(%s2)
^1(%t: !state):
  ret %t
}
)";
// The same dataflow, emitted in another order, with other names and the blocks reversed.
constexpr std::string_view order_b = R"(
func @5 {
^last(%t: !state):
  ret %t
^first(%st: !state, %p: T1, %q: T1):
  %minus: T1 = sub %p, %q
  %plus: T1 = add %p, %q
  %s1: !state = store %st, %p, %times [T1, /]
  %times: T1 = mul %plus, %p
  %s2: !state = store %s1, %p, %minus [T1, /]
  br ^last(%s2)
}
)";

void canonical_order() {
  // order_b names `^last` first, so it parses as the entry; move the entry first instead.
  std::string b(order_b);
  auto last = b.find("^last");
  auto first = b.find("^first");
  b = b.substr(0, last) + b.substr(first, b.rfind('}') - first) + b.substr(last, first - last) + "}\n";
  auto fa = parse_one(order_a), fb = parse_one(b);
  check(fa.body == fb.body, "emission order and names do not change the body:\n" +
                                print(fa.name, fa.body) + print(fb.name, fb.body));
  check(fingerprint(fa.body) == fingerprint(fb.body), "nor the fingerprint");

  // Any change of meaning changes the fingerprint.
  auto changed = [&](std::string_view from, std::string_view to) {
    std::string t(order_a);
    t.replace(t.find(from), from.size(), to);
    return fingerprint(parse_one(t).body) != fingerprint(fa.body);
  };
  check(changed("add %x", "add nsw %x"), "flags are hashed");
  check(changed("mul %a, %x", "mul %x, %a"), "operand order is hashed");
  check(changed("[T1, /]\n  %s2", "[T1, /f0]\n  %s2"), "paths are hashed");
  check(changed("%x: T1, %y", "%x: T9, %y"), "types are hashed");

  // Handles are hashed through their fingerprints, not their numbers.
  struct same_types : stand_in_handles {
    using stand_in_handles::operator();
    fingerprint128 operator()(QualType t) const { return of(2, raw(t) == 9 ? 1 : raw(t)); }
  };
  std::string t9(order_a);
  t9.replace(t9.find("%x: T1"), 6, "%x: T9");
  check(fingerprint(parse_one(t9).body, same_types{}) == fingerprint(fa.body, same_types{}),
        "equal handle fingerprints give equal bodies' fingerprints");
}

// A reader of a state comes before the node that moves the state on, whatever the emission order.
void readers_first() {
  auto f = parse_one(R"(
func @6 {
^0(%s: !state, %p: T1):
  %s1: !state = obj.end %s, %p
  %x: T1 = load %s, %p [T1, /]
  ret %s1, %x
}
)");
  check(f.body.node(NodeId(0)).op == Op::load && f.body.node(NodeId(1)).op == Op::obj__end,
        "the load is placed before obj.end:\n" + print(f.name, f.body));
}

void out_of_range_case_value() {
  Builder b;
  BlockId entry = b.add_block({Ty::state(), Ty::of(QualType(1))});
  BlockId other = b.add_block({});
  b.emit(other, Op::unreachable, {}, {});
  std::uint64_t imms[] = {1, 5'000'000'000u};
  std::vector<SuccRef> succs{{other, {}}, {other, {}}};
  ValueId v = b.param(entry, 1);
  b.emit(entry, Op::switch_, 0, {}, std::span(&v, 1), imms, succs);
  auto built = b.finish();
  check(built && !verify(built->function), "a case value beyond 32 bits is rejected");
}

void builder_and_debug() {
  Builder b;
  BlockId entry = b.add_block({Ty::state(), Ty::of(QualType(1))});
  BlockId exit = b.add_block({Ty::state()});
  BlockId dead = b.add_block({Ty::state()});
  ValueId s = b.param(entry, 0), x = b.param(entry, 1);
  NodeId unused = b.emit(entry, Op::neg, {Ty::of(QualType(1))}, {x});           // emitted first
  NodeId sum = b.emit(entry, Op::add, {Ty::of(QualType(1))}, {x, x}, {}, bit(Flag::nsw));
  NodeId st = b.emit(entry, Op::store, {Ty::state()}, {s, x, b.result(sum)}, {1, 0});
  b.emit(dead, Op::unreachable, {}, {});
  b.emit(exit, Op::ret, {}, {b.param(exit, 0)});
  NodeId br = b.emit(entry, Op::br, 0, {}, {}, {}, std::vector<SuccRef>{{exit, {b.result(st)}}});
  auto built = b.finish();
  check(built.has_value(), "finish");
  if (!built) return;
  const Function& f = built->function;
  check(verify(f).has_value(), "verify");
  check(f.blocks.size() == 2, "the unreachable block is dropped");
  check(raw(built->node_of[raw(unused)]) == 0 && raw(built->node_of[raw(sum)]) == 1 &&
            raw(built->node_of[raw(st)]) == 2 && raw(built->node_of[raw(br)]) == 3,
        "the unused root first, then the terminator's operands in operand order");
  check(raw(built->node_of[3]) == none, "the dead node is dropped");

  FunctionDebug emitted{.loc = {{ItemId(7), 0}, {ItemId(7), 1}, {ItemId(7), 2}, {ItemId(7), 9}, {ItemId(7), 3}, {ItemId(7), 4}},
                        .vars = {{LocalId(0), {st, unused}}}};
  FunctionDebug d = emitted.remap(built->node_of, std::uint32_t(f.nodes.size()));
  check(d.loc.size() == 5 && d.loc[2] == Loc{ItemId(7), 2} && d.loc[3] == Loc{ItemId(7), 4} &&
            d.loc[4] == Loc{ItemId(7), 3},
        "locations follow their nodes");
  check(d.vars[0].nodes == std::vector{NodeId(0), NodeId(2)}, "variables follow their nodes");

  // Origin maps: L1 -> L2 -> L3, composed.
  EntityId fn{42}, callee{43};
  std::vector<std::vector<Origin>> l2_sets{{{fn, NodeId(1)}}, {{fn, NodeId(2)}, {callee, NodeId(0)}}};
  OriginMap l2(l2_sets);
  OriginMap callee_l2 = OriginMap::identity(callee, 1);
  std::vector<std::vector<Origin>> l3_sets{{{fn, NodeId(1)}, {fn, NodeId(0)}}, {}};
  OriginMap l3(l3_sets);
  OriginMap l2_of_fn = l2;
  auto composed = OriginMap::compose(l3, [&](EntityId e) -> const OriginMap* {
    return e == fn ? &l2_of_fn : e == callee ? &callee_l2 : nullptr;
  });
  check(composed.size() == 2, "one set per node");
  auto o = composed.of(NodeId(0));
  check(std::vector(o.begin(), o.end()) ==
            std::vector<Origin>{{fn, NodeId(1)}, {fn, NodeId(2)}, {callee, NodeId(0)}},
        "composition unions, sorts and deduplicates the origins");
  check(composed.of(NodeId(1)).empty(), "a node without origin keeps none");
}

void errors() {
  auto has = [](std::string_view text, std::string_view what) {
    std::string e = parse_error(text);
    check(e.find(what) != std::string::npos, std::format("expected an error with `{}`, got `{}`", what, e));
  };
  has("func @1 {\n^0(%s: !state):\n  frob %s\n}", "unknown op `frob`");
  has("func @1 {\n^0(%s: !state):\n  ret %t\n}", "%t is not defined");
  has("func @1 {\n^0(%s: !state):\n  br ^9\n}", "block ^9 is not defined");
  has("func @1 {\n^0(%s: !state, %v: T1):\n  %x: T1 = add %s, %v\n  ret %s\n}", "wrong kind");
  has("func @1 {\n^0(%s: !state, %v: T1):\n  %x: T1 = sub nsw exact %v, %v\n  ret %s\n}", "flag");
  has("func @1 {\n^0(%s: !state):\n  %x: T1 = const [#1]\n}", "terminator");
  has("func @1 {\n^0(%s: !state, %v: T1):\n  cond_br %v ^1, ^2\n^1:\n  %x: T1 = neg %v\n  br ^3\n"
      "^2:\n  br ^3\n^3:\n  ret %s, %x\n}",
      "does not dominate");
  has("func @1 {\n^0(%s: !state, %v: T1):\n  br ^1(%v)\n^1(%w: T2):\n  ret %s\n}", "wrong type");
  has("func @1 {\n^0(%s: !state, %v: T1):\n  %s1: !state = invoke %s [@3] ^1, ^2\n^1:\n  unreachable\n^2(%a: T1, %b: T1):\n  unreachable\n}",
      "implicit parameters");
  has("func @1 {\n^0(%s: !state, %v: T1):\n  %a: T1 = add %v, %b\n  %b: T1 = add %a, %v\n  ret %s\n}",
      "cycle");
  has("func @1 {\n^0(%s: !state, %v: T1):\n  switch %v [(#1, #2)] ^0(%s, %v), ^0(%s, %v)\n}", "case value");
  has("func @1 {\n^0(%s: !state, %p: T1):\n  %a: !state = obj.end %s, %p\n  %b: !state = obj.end %s, %p\n"
      "  ret %b\n}",
      "moves %0 after node 0");
  has("func @1 {\n^0(%s: !state, %p: T1):\n  %a: !state = obj.end %s, %p\n  br ^1\n^1:\n"
      "  %x: T1 = load %s, %p [T1, /]\n  ret %a, %x\n}",
      "after node");
  // A state from before a loop, moved on inside it, would be moved on again by the next iteration.
  has("func @1 {\n^0(%s: !state, %p: T1):\n  br ^1\n^1:\n  %a: !state = obj.end %s, %p\n"
      "  %c: T1 = load %a, %p [T1, /]\n  cond_br %c ^1, ^2\n^2:\n  ret %a\n}",
      "moves %0 after node");
  // Review findings on #23.
  has("func @1 {\n^0(%s: !state, %c: T1):\n  cond_br %c ^a, ^b\n^a:\n  %s1: !state, %r: T1 = invoke %s [@3] ^n, ^u\n"
      "^b:\n  br ^n\n^n:\n  ret %s1, %r\n^u(%us: !state, %e: !exn):\n  unreachable\n}",
      "no other predecessor");
  has("func @1 {\n^0(%s: !state):\n  %y: T1 = neg %x\n  br ^1\n^1:\n  %x: T1 = unreachable\n}", "node 0");
  has("func @1 {\n^0(%s: !state):\n  br ^1(%s, %s)\n^1(%a: !state, %b: !state):\n  ret %a\n}", "more than one !state");
  has("func @1 {\n^0(%s: !state):\n  %t: !state = invoke %s [@3] ^1(%s), ^2\n^1(%a: !state):\n  ret %a\n"
      "^2(%u: !state, %e: !exn):\n  unreachable\n}",
      "consumes a state twice");
  has("func @1 {\n^0(%s: !state):\n  br ^1(%s)\n^1(%a: !state):\n  br ^0(%a)\n}", "entry block");
}

// --- The evaluator hook -----------------------------------------------------------------------

struct Machine {
  std::vector<std::int64_t> values;
};
using EvalFn = void(Machine&, const Function&, NodeId);

struct ToyEvaluator {
  static void add(Machine& m, const Function& f, NodeId n) {
    auto ops = f.operands_of(n);
    m.values[raw(*f.results_of(n).begin())] = m.values[raw(ops[0])] + m.values[raw(ops[1])];
  }
  static void neg(Machine& m, const Function& f, NodeId n) {
    m.values[raw(*f.results_of(n).begin())] = -m.values[raw(f.operands_of(n)[0])];
  }
};

void evaluator_hook() {
  constexpr auto table = eval_table<ToyEvaluator, EvalFn, false>();
  static_assert(table[std::to_underlying(Op::add)] != nullptr && table[std::to_underlying(Op::sub)] == nullptr);
  auto f = parse_one("func @1 {\n^0(%s: !state, %v: T1):\n  %a: T1 = add %v, %v\n  %b: T1 = neg %a\n  ret %s, %b\n}");
  Machine m{std::vector<std::int64_t>(f.body.values.size())};
  m.values[1] = 21;
  for (NodeId n : f.body.nodes_of(BlockId(0)))
    if (auto fn = table[std::to_underlying(f.body.node(n).op)]) fn(m, f.body, n);
  check(m.values[3] == -42, "the hook dispatches by op");
}

}  // namespace

int main() {
  table_checks();
  every_op();
  hand_written();
  canonical_order();
  readers_first();
  out_of_range_case_value();
  builder_and_debug();
  errors();
  evaluator_hook();
  return ycxx::unit::result("ir");
}
