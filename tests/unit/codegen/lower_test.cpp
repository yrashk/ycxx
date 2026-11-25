// `lower`, L1 to L2 (#26, the α subset): golden L2 for C-like bodies written in L1's text form,
// `l2::verify` on every result, and an error naming the node for what the subset leaves out.
import std;
import ycxx.ir;
import ycxx.ir.l2;
import ycxx.ir.legalize;
import ycxx.ir.lower;
import ycxx.unit;
import ycxx.unit.codegen;

using ycxx::unit::check;
namespace ir = ycxx::ir;
namespace l2 = ycxx::l2;

namespace {

std::expected<std::string, std::string> lower_text(std::string_view text) {
  auto fs = ir::parse(text);
  if (!fs) return std::unexpected(std::format("line {}: {}", fs.error().line, fs.error().message));
  std::string out;
  for (const auto& f : *fs) {
    if (auto v = ir::verify(f.body); !v) return std::unexpected("L1: " + v.error());
    auto l = ir::lower(f.body, f.name, ycxx::unit::TestLayout{});
    if (!l) return std::unexpected(std::format("node {}: {}", ir::raw(l.error().node), l.error().message));
    if (auto v = l2::verify(*l); !v) return std::unexpected("L2: " + v.error());
    out += l2::print(f.name, *l);
  }
  return out;
}

void golden(std::string_view what, std::string_view l1, std::string_view want) {
  auto got = lower_text(l1);
  if (!got) {
    check(false, std::format("{}: {}", what, got.error()));
    return;
  }
  check(*got == want, std::format("{}: lowers to\n{}\nwant\n{}", what, *got, want));
}

void fails(std::string_view what, std::string_view l1, std::string_view message) {
  auto got = lower_text(l1);
  check(!got && got.error().contains(message),
        std::format("{}: fails with `{}`, got {}", what, message, got ? *got : got.error()));
}

void test_scalars() {
  // int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }, as branches.
  golden("fact", R"(
func @2 {
^0(%s: !state, %n: T1):
  %one: T1 = const [#1]
  %c: T2 = icmp %n, %one [sle]
  cond_br %c ^1, ^2
^1:
  ret %s, %one
^2:
  %m: T1 = sub nsw %n, %one
  %s3: !state, %r: T1 = call %s, %m [@2, 0, 0, 0]
  %p: T1 = mul nsw %n, %r
  ret %s3, %p
}
)",
         R"(func @2 {
^0(%0: !state, %1: i32):
  %2: i32 = const [1]
  %3: i1 = icmp %1, %2 [sle]
  cond_br %3 ^2, ^1
^1:
  %4: i32 = sub nsw %1, %2
  %5: !state, %6: i32 = call %0, %4 [@2, 1, 0, 0]
  %7: i32 = mul nsw %1, %6
  ret %5, %7
^2:
  ret %0, %2
}
)");

  // Narrow parameters and arguments keep their width, and the call records which arguments are
  // signed (the mask after the callee); a narrow result is returned extended to 32 bits and
  // truncated by the caller; a bool is zero-extended.
  golden("narrow", R"(
func @3 {
^0(%s: !state, %c: T5, %b: T2, %u: T10):
  %w: T1 = iconv %c
  %x: T4 = iconv %u
  %s1: !state, %r: T5 = call %s, %c, %b [@4, 0, 0, 0]
  %t: T2 = tobool %w
  %v: T13 = iconv %x
  ret %s1, %t
}
)",
         R"(func @3 {
^0(%0: !state, %1: i8, %2: i1, %3: i8):
  %4: i32 = zext %3
  %5: i16 = trunc %4
  %6: !state, %7: i32 = call %0, %1, %2 [@4, 1, 0, 0]
  %8: i8 = trunc %7
  %9: i32 = sext %1
  %10: i32 = const [0]
  %11: i1 = icmp %9, %10 [ne]
  %12: i32 = zext %11
  ret %6, %12
}
)");
}

void test_memory() {
  // Locals: alloc, erroneous initialization as zero stores, paths as byte offsets, bool as a byte.
  golden("locals", R"(
func @5 {
^0(%s0: !state, %v: T1, %f: T2):
  %s1: !state, %p: T12 = alloc %s0 [T11, automatic, erroneous]
  %s2: !state, %q: T16 = alloc %s1 [T2, automatic, indeterminate]
  %s3: !state = obj.begin %s2, %p [T11]
  %s4: !state = store %s3, %p, %v [T1, /f0]
  %w: T3 = iconv %v
  %s5: !state = store %s4, %p, %w [T3, /f1]
  %s6: !state = store %s5, %q, %f [T2, /]
  %x: T3 = load %s6, %p [T3, /f1]
  %g: T2 = load %s6, %q [T2, /]
  %s7: !state = obj.end %s6, %p
  %y: T1 = iconv %x
  ret %s7, %y
}
)",
         R"(func @5 {
^0(%0: !state, %1: i32, %2: i1):
  %3: ptr = alloca [16, 8, 0]
  %4: i64 = const [0]
  %5: !state = store %0, %3, %4 [T11]
  %6: i64 = const [8]
  %7: ptr = ptradd %3, %6
  %8: i64 = const [0]
  %9: !state = store %5, %7, %8 [T11]
  %10: ptr = alloca [1, 1, 1]
  %11: !state = store %9, %3, %1 [T1]
  %12: i64 = sext %1
  %13: i64 = const [8]
  %14: ptr = ptradd %3, %13
  %15: !state = store %11, %14, %12 [T3]
  %16: i8 = zext %2
  %17: !state = store %15, %10, %16 [T2]
  %18: i64 = const [8]
  %19: ptr = ptradd %3, %18
  %20: i64 = load %17, %19 [T3]
  %21: i8 = load %17, %10 [T2]
  %22: i1 = trunc %21
  %23: i32 = trunc %20
  ret %17, %23
}
)");

  // Globals, string literals and pointer arithmetic.
  golden("pointers", R"(
func @6 {
^0(%s: !state, %i: T1):
  %g: T9 = global [@100]
  %p: T6 = ptr_member %g [T6, /e0]
  %q: T6 = ptr_add %p, %i [T5]
  %c: T5 = load %s, %q [T5, /]
  %d: T3 = ptr_diff %q, %p [T5]
  %n: T7 = ptr_add %g, %i [T1]
  %e: T2 = ptr_cmp %p, %q [lt]
  %z: T2 = tobool %p
  %s1: !state, %r: T1 = call %s, %p [@101, 0, 0, 0]
  ret %s1, %r
}
)",
         R"(func @6 {
^0(%0: !state, %1: i32):
  %2: ptr = symbol [@100]
  %3: i64 = sext %1
  %4: ptr = ptradd %2, %3
  %5: i64 = ptr2int %4
  %6: i64 = ptr2int %2
  %7: i64 = sub nsw %5, %6
  %8: i64 = sext %1
  %9: i64 = const [4]
  %10: i64 = mul nsw %8, %9
  %11: ptr = ptradd %2, %10
  %12: i1 = icmp %2, %4 [ult]
  %13: ptr = const [0]
  %14: i1 = icmp %2, %13 [ne]
  %15: i8 = load %0, %4 [T5]
  %16: !state, %17: i32 = call %0, %2 [@101, 0, 0, 0]
  ret %16, %17
}
)");
}

void test_control() {
  // Block arguments carry loop state; `consteval_br` takes its runtime edge and the consteval
  // block is dropped; `is_const_eval` is false.
  golden("loop", R"(
func @7 {
^0(%s: !state, %n: T1):
  %z: T1 = const [#0]
  consteval_br ^9, ^1(%z, %z)
^9:
  unreachable
^1(%i: T1, %acc: T1):
  %c: T2 = icmp %i, %n [sle]
  cond_br %c ^2, ^3(%acc)
^2:
  %acc2: T1 = add nsw %acc, %i
  %one: T1 = const [#1]
  %i2: T1 = add nsw %i, %one
  %k: T2 = is_const_eval
  br ^1(%i2, %acc2)
^3(%r: T1):
  ret %s, %r
}
)",
         R"(func @7 {
^0(%0: !state, %1: i32):
  %2: i32 = const [0]
  br ^1(%2, %2)
^1(%3: i32, %4: i32):
  %5: i1 = icmp %3, %1 [sle]
  cond_br %5 ^3, ^2(%4)
^2(%6: i32):
  ret %0, %6
^3:
  %7: i1 = const [0]
  %8: i32 = const [1]
  %9: i32 = add nsw %3, %8
  %10: i32 = add nsw %4, %3
  br ^1(%9, %10)
}
)");
}

enum class InvokeGraph { valid, extra_landing_edge, normal_parameter_on_unwind, normal_computation_on_unwind };

l2::Function invoke_graph(InvokeGraph variant) {
  using enum l2::Type;
  using l2::Op;
  using Edge = l2::Builder::SuccArgs;

  l2::Builder b;
  constexpr ir::BlockId call_block{1}, normal_block{2}, landing_block{3}, handler_block{4}, ordinary_block{5};
  const l2::Type inputs[] = {state, i32, i1, ptr, i64};
  const l2::Type normal_types[] = {state, i32};
  const l2::Type landing_types[] = {state, ptr, i64};
  const l2::Type handler_types[] = {state};

  auto entry = b.begin_block(inputs);
  const ir::ValueId condition[] = {b.param(entry, 2)};
  const Edge branches[] = {{call_block, {b.param(entry, 0), b.param(entry, 1)}},
                           {ordinary_block, {b.param(entry, 0), b.param(entry, 3), b.param(entry, 4)}}};
  b.emit(Op::cond_br, {}, condition, {}, 0, branches);

  auto call = b.begin_block(normal_types);
  const ir::ValueId operands[] = {b.param(call, 0), b.param(call, 1)};
  const std::uint64_t imms[] = {7, 1, 0, 0};
  const Edge exits[] = {{normal_block, {}}, {landing_block, {}}};
  b.emit(Op::invoke, {}, operands, imms, 0, exits);

  auto normal = b.begin_block(normal_types);
  auto result = b.param(normal, 1);
  auto zero = b.value(Op::const_, i32, {}, {0});
  auto computed = b.value(Op::add, i32, {result, zero});
  const ir::ValueId succeeded[] = {b.param(normal, 0), computed};
  b.emit(Op::ret, {}, succeeded);

  auto landing = b.begin_block(landing_types);
  const Edge handle[] = {{handler_block, {b.param(landing, 0)}}};
  b.emit(Op::br, {}, {}, {}, 0, handle);

  auto handler = b.begin_block(handler_types);
  // Both normal definitions precede this block in the builder, but the unwind path bypasses them.
  auto recovered = b.value(Op::const_, i32, {}, {47});
  if (variant == InvokeGraph::normal_parameter_on_unwind) recovered = result;
  else if (variant == InvokeGraph::normal_computation_on_unwind) recovered = computed;
  const ir::ValueId handled[] = {b.param(handler, 0), recovered};
  b.emit(Op::ret, {}, handled);

  auto ordinary = b.begin_block(landing_types);
  if (variant == InvokeGraph::extra_landing_edge) {
    const Edge extra[] = {{landing_block, {b.param(ordinary, 0), b.param(ordinary, 1), b.param(ordinary, 2)}}};
    b.emit(Op::br, {}, {}, {}, 0, extra);
  } else {
    const ir::ValueId bypassed[] = {b.param(ordinary, 0), b.value(Op::const_, i32, {}, {59})};
    b.emit(Op::ret, {}, bypassed);
  }
  return b.finish({i32});
}

void test_invoke_verification() {
  auto valid = l2::verify(invoke_graph(InvokeGraph::valid));
  check(valid.has_value(), valid ? "verify invoke with exclusive normal and unwind entries" : valid.error());
  check(!l2::verify(invoke_graph(InvokeGraph::extra_landing_edge)),
        "reject a reachable ordinary edge with correctly typed arguments into the landing block");
  check(!l2::verify(invoke_graph(InvokeGraph::normal_parameter_on_unwind)),
        "reject using the normal block parameter on the unwind path");
  check(!l2::verify(invoke_graph(InvokeGraph::normal_computation_on_unwind)),
        "reject using a value computed in the normal block on the unwind path");
}

void test_errors() {
  fails("dynamic", "func @1 {\n^0(%s: !state):\n  %s1: !state, %p: T7 = alloc %s [T1, dynamic, zero]\n  ret %s1\n}",
        "dynamic storage is not in the α subset");
  fails("aggregate load", "func @1 {\n^0(%s: !state, %p: T12):\n  %x: T11 = load %s, %p [T11, /]\n  ret %s\n}",
        "not a scalar type");
  fails("switch", "func @1 {\n^0(%s: !state, %v: T1):\n  switch %v [(#1)] ^1, ^1\n^1:\n  ret %s\n}",
        "`switch` is not in the α subset");
}

// #1172: `lower` writes `i128` for every target, and `legalize` expands what the target lacks.
constexpr ir::EntityId first_runtime{900};

std::optional<l2::Function> lower_one(std::string_view text) {
  auto fs = ir::parse(text);
  if (!fs) return std::nullopt;
  auto l = ir::lower((*fs)[0].body, (*fs)[0].name, ycxx::unit::TestLayout{});
  if (!l) return std::nullopt;
  return std::move(*l);
}
std::string legalized(std::string_view text, const ir::Int128Support& target) {
  auto l = lower_one(text);
  if (!l) return "does not lower";
  auto out = ir::legalize(std::move(*l), target, [](ir::Runtime r) {
    return ir::EntityId(ir::raw(first_runtime) + std::uint32_t(std::to_underlying(r)));
  });
  if (!out) return out.error().message;
  if (auto v = l2::verify(*out); !v) return "L2: " + v.error();
  return l2::print(ir::EntityId(1), *out);
}
void golden_legal(std::string_view what, std::string_view l1, const ir::Int128Support& target, std::string_view want) {
  const std::string got = legalized(l1, target);
  check(got == want, std::format("{}: legalizes to\n{}\nwant\n{}", what, got, want));
}

void test_int128() {
  constexpr std::string_view add = R"(
func @1 {
^0(%s: !state, %p: T23, %q: T23):
  %a: T22 = load %s, %p [T22, /]
  %b: T22 = load %s, %q [T22, /]
  %c: T22 = add nsw %a, %b
  %d: T22 = sdiv %c, %b
  %s1: !state = store %s, %p, %d [T22, /]
  ret %s1
}
)";
  // `lower` is the same for every target.
  golden("i128", add, R"(func @1 {
^0(%0: !state, %1: ptr, %2: ptr):
  %3: i128 = load %0, %1 [T22]
  %4: i128 = load %0, %2 [T22]
  %5: i128 = add nsw %3, %4
  %6: i128 = sdiv %5, %4
  %7: !state = store %0, %1, %6 [T22]
  ret %7
}
)");
  // A target with 64-bit registers: halves, a carry, and the runtime's division in the state chain.
  golden_legal("i128, 64-bit target", add, ir::Int128Support{}, R"(func @1 {
^0(%0: !state, %1: ptr, %2: ptr):
  %3: i64 = load %0, %1 [T22]
  %4: i64 = const [8]
  %5: ptr = ptradd %1, %4
  %6: i64 = load %0, %5 [T22]
  %7: i64 = load %0, %2 [T22]
  %8: i64 = const [8]
  %9: ptr = ptradd %2, %8
  %10: i64 = load %0, %9 [T22]
  %11: i64 = add %3, %7
  %12: i1 = icmp %11, %3 [ult]
  %13: i64 = zext %12
  %14: i64 = add %6, %10
  %15: i64 = add %14, %13
  %16: !state, %17: i64, %18: i64 = call %0, %11, %15, %7, %10 [@900, 0, 0, 0]
  %19: !state = store %16, %1, %17 [T22]
  %20: i64 = const [8]
  %21: ptr = ptradd %1, %20
  %22: !state = store %19, %21, %18 [T22]
  ret %22
}
)");
  // A target with 128-bit registers, addition and memory access, but no division: what it has is
  // untouched, and the division takes its operands apart and joins its result.
  ir::Int128Support native{.values = true};
  for (l2::Op op : {l2::Op::add, l2::Op::load, l2::Op::store}) native.ops.set(std::to_underlying(op));
  golden_legal("i128, native", add, native, R"(func @1 {
^0(%0: !state, %1: ptr, %2: ptr):
  %3: i128 = load %0, %1 [T22]
  %4: i128 = load %0, %2 [T22]
  %5: i128 = add nsw %3, %4
  %6: i64 = trunc %5
  %7: i64 = high %5
  %8: i64 = trunc %4
  %9: i64 = high %4
  %10: !state, %11: i64, %12: i64 = call %0, %6, %7, %8, %9 [@900, 0, 0, 0]
  %13: i128 = pair %11, %12
  %14: !state = store %10, %1, %13 [T22]
  ret %14
}
)");
  // With everything native, nothing changes.
  ir::Int128Support all{.values = true};
  all.ops.set();
  if (auto l = lower_one(add))
    check(legalized(add, all) == l2::print(ir::EntityId(1), *l), "i128, all native: legalizes to itself");

  // Block arguments: two each without 128-bit registers, whole with them.
  constexpr std::string_view join = R"(
func @1 {
^0(%s: !state, %p: T23, %c: T2):
  %a: T22 = load %s, %p [T22, /]
  cond_br %c ^1(%s, %a), ^2(%s)
^1(%s1: !state, %x: T22):
  %s2: !state = store %s1, %p, %x [T22, /]
  ret %s2
^2(%s3: !state):
  %k: T22 = const [#2130706944]
  br ^1(%s3, %k)
}
)";
  check(legalized(join, ir::Int128Support{}).contains("(%") && !legalized(join, ir::Int128Support{}).contains("i128"),
        "i128 block argument: no i128 is left for a 64-bit target");
  check(legalized(join, ir::Int128Support{}).contains("const [1]"), "a constant's high half is kept");
  check(legalized(join, native).contains("i128)"), "i128 block argument: whole for a 128-bit target");

  // A 128-bit parameter, argument and result pass in two words (both psABIs), whatever the
  // arithmetic the target has.
  golden("i128 call", R"(
func @1 {
^0(%s: !state, %a: T22, %n: T3):
  %s1: !state, %r: T22 = call %s, %n, %a [@1, 0, 0, 0]
  ret %s1, %r
}
)",
         R"(func @1 {
^0(%0: !state, %1: i64, %2: i64, %3: i64):
  %4: i128 = pair %1, %2
  %5: i64 = trunc %4
  %6: i64 = high %4
  %7: !state, %8: i64, %9: i64 = call %0, %3, %5, %6 [@1, 1, 0, 0]
  %10: i128 = pair %8, %9
  %11: i64 = trunc %10
  %12: i64 = high %10
  ret %7, %11, %12
}
)");
}

}  // namespace

int main() {
  test_scalars();
  test_memory();
  test_control();
  test_invoke_verification();
  test_errors();
  test_int128();
  return ycxx::unit::result("unit.lower");
}
