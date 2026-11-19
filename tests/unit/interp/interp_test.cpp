// Unit tests of the L1 interpreter (#25): bodies written in L1's text form, with `{key}`
// placeholders for handles, run in constant and runtime mode against a host of test tables. Each
// [expr.const] check has a body that breaks it and must fail with its reason, and the body that
// stays within it must give the value.
import std;
import ycxx.ast;
import ycxx.ir;
import ycxx.interp;
import ycxx.unit;

namespace ast = ycxx::ast;
namespace ir = ycxx::ir;
namespace interp = ycxx::interp;
using ast::ConstId;
using ast::EntityId;
using ast::QualType;
using interp::Reason;
using ycxx::unit::check;

namespace {

const ast::Target& target = ast::aarch64_darwin;

struct Host {
  std::map<std::uint32_t, interp::Callee> fns;
  std::map<std::uint32_t, interp::GlobalVar> globals;
  std::map<std::uint32_t, int> body_fetches;
  bool stop = false;

  interp::ready<interp::Callee> body(EntityId e) {
    ++body_fetches[std::to_underlying(e)];
    auto it = fns.find(std::to_underlying(e));
    return {it == fns.end() ? interp::Callee{} : it->second};
  }
  interp::ready<std::optional<interp::GlobalVar>> global(EntityId e) {
    auto it = globals.find(std::to_underlying(e));
    if (it == globals.end()) return {std::nullopt};
    return {it->second};
  }
  bool stop_requested() const { return stop; }
};
static_assert(interp::Host<Host>);

// The test's universe. `{key}` in a body is a handle: a type by name (`{int}`, `{int*}`,
// `{int[3]}`), an entity (`{@name}`, made on first use), or a constant (`{i5}` an int, `{u5}` an
// unsigned, `{b1}` a bool, `{null}`, `{s}` the address of the string literal "hi").
struct World {
  ast::Interners in;
  std::map<std::string, std::string> keys;
  std::map<std::string, EntityId> entities;
  QualType int_t, uint_t, bool_t, char_t, cchar_t, long_t, cint_t;
  ConstId hi;  // the string literal object "hi"

  void type(std::string name, QualType q) { keys["{" + name + "}"] = std::format("T{}", std::to_underlying(q)); }

  World() {
    using enum ast::Builtin;
    int_t = ast::builtin(in, int_);
    uint_t = ast::builtin(in, unsigned_int);
    bool_t = ast::builtin(in, bool_);
    char_t = ast::builtin(in, char_);
    long_t = ast::builtin(in, long_);
    cchar_t = ast::builtin(in, char_, ast::Cv::const_);
    cint_t = ast::builtin(in, int_, ast::Cv::const_);
    auto arr = [&](QualType e, std::int64_t n) {
      return ast::array_of(in, target, e, ast::int_value(in, target, unsigned_long, n));
    };
    type("int", int_t);
    type("uint", uint_t);
    type("bool", bool_t);
    type("char", char_t);
    type("long", long_t);
    type("llong", ast::builtin(in, long_long));
    type("long*", ast::pointer_to(in, long_t));
    type("cint", cint_t);
    type("cchar", cchar_t);
    type("int*", ast::pointer_to(in, int_t));
    type("void*", ast::pointer_to(in, ast::builtin(in, void_)));
    type("char*", ast::pointer_to(in, char_t));
    type("cchar*", ast::pointer_to(in, cchar_t));
    type("int**", ast::pointer_to(in, ast::pointer_to(in, int_t)));
    type("int[3]", arr(int_t, 3));
    {
      std::array<QualType, 1> ps{int_t};
      type("int(*)(int)", ast::pointer_to(in, ast::function_type(in, int_t, std::span<const QualType>(ps))));
    }
    type("int[3]*", ast::pointer_to(in, arr(int_t, 3)));
    type("int[2][3]", arr(arr(int_t, 3), 2));
    type("int[2][3]*", ast::pointer_to(in, arr(arr(int_t, 3), 2)));
    QualType hi_t = arr(cchar_t, 3);
    type("cchar[3]", hi_t);
    type("cchar[3]*", ast::pointer_to(in, hi_t));
    std::array<std::uint8_t, 3> units{'h', 'i', 0};
    hi = in.intern_value(ast::VString{hi_t, in.intern_seq(std::span<const std::uint8_t>(units))});
    ConstId addr = in.intern_value(ast::VLvalue{hi, in.intern_seq(std::span<const std::uint32_t>{}), 0});
    keys["{s}"] = std::format("#{}", std::to_underlying(addr));
    std::array<std::uint8_t, 3> xy{'x', 'y', 0};
    ConstId xy_s = in.intern_value(ast::VString{hi_t, in.intern_seq(std::span<const std::uint8_t>(xy))});
    keys["{s2}"] = std::format("#{}", std::to_underlying(in.intern_value(ast::VLvalue{xy_s, in.intern_seq(std::span<const std::uint32_t>{}), 0})));
    std::array<std::uint8_t, 3> ci{'c', 'i', 0};
    ConstId ci_s = in.intern_value(ast::VString{hi_t, in.intern_seq(std::span<const std::uint8_t>(ci))});
    keys["{s3}"] = std::format("#{}", std::to_underlying(in.intern_value(ast::VLvalue{ci_s, in.intern_seq(std::span<const std::uint32_t>{}), 0})));
    keys["{null}"] = std::format("#{}", std::to_underlying(in.intern_value(ast::VNullptr{})));
  }

  EntityId entity(const std::string& name) {
    if (auto it = entities.find(name); it != entities.end()) return it->second;
    EntityId e = in.entities.add<EntityId>(ast::seed("interp test entity " + name));
    entities.emplace(name, e);
    return e;
  }

  std::string expand(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
      std::size_t close = text[i] == '{' ? text.find_first_of("} \n", i) : std::string_view::npos;
      if (close == std::string_view::npos || text[close] != '}') {
        out += text[i++];
        continue;
      }
      std::string key(text.substr(i, close - i + 1));
      std::string inner = key.substr(1, key.size() - 2);
      if (auto it = keys.find(key); it != keys.end()) {
        out += it->second;
      } else if (inner.starts_with('@')) {
        out += std::format("@{}", std::to_underlying(entity(inner.substr(1))));
      } else {
        QualType t = inner[0] == 'i' ? int_t : inner[0] == 'u' ? uint_t : inner[0] == 'b' ? bool_t : long_t;
        std::int64_t v = std::stoll(inner.substr(1));
        out += std::format("#{}", std::to_underlying(ast::int_value(in, target, t, v)));
      }
      i = close + 1;
    }
    return out;
  }
};

struct Run {
  World w;
  Host host;
  std::vector<ir::NamedFunction> fns;

  // Parses the bodies; the first is the entry. Every function is constexpr unless named in
  // `plain`.
  explicit Run(std::string_view text, std::set<std::string> plain = {},
               std::function<void(World&)> setup = {}) {
    if (setup) setup(w);
    auto parsed = ir::parse(w.expand(text));
    if (!parsed) {
      check(false, std::format("parse error {}:{}: {}", parsed.error().line, parsed.error().column, parsed.error().message));
      return;
    }
    fns = std::move(*parsed);
    for (auto& f : fns) {
      auto v = ir::verify(f.body);
      check(v.has_value(), "the test body verifies: " + (v ? std::string() : v.error()));
    }
    for (auto& [name, e] : w.entities)
      for (auto& f : fns)
        if (f.name == e) host.fns[std::to_underlying(e)] = {&f.body, !plain.contains(name)};
  }

  interp::Outcome go(std::vector<interp::Value> args = {}, interp::Config c = {}) {
    if (fns.empty()) return {};
    return interp::evaluate(host, w.in, target, fns[0].body, fns[0].name, std::move(args), c).run();
  }
  interp::Outcome runtime(std::vector<interp::Value> args = {}) {
    return go(std::move(args), {.mode = interp::Mode::runtime});
  }
};

interp::Value i(std::int64_t v) { return interp::Value::integer(std::uint64_t(v)); }

void expect_value(const interp::Outcome& o, std::int64_t v, std::string_view what,
                  std::source_location at = std::source_location::current()) {
  bool ok = o.status == interp::Outcome::Status::value && o.values.size() == 1 &&
            o.values[0].kind == interp::Result::Kind::integer && std::int64_t(o.values[0].bits) == v;
  std::string got = o.status == interp::Outcome::Status::failed ? interp::message(o.failure)
                    : o.values.empty()                         ? std::string("no value")
                                                               : std::format("{}", std::int64_t(o.values[0].bits));
  check(ok, std::format("{}: expected {}, got {}", what, v, got), at);
}

void expect_failure(const interp::Outcome& o, Reason r, std::string_view what,
                    std::source_location at = std::source_location::current()) {
  bool ok = o.status == interp::Outcome::Status::failed && o.failure.reason == r;
  std::string got = o.status == interp::Outcome::Status::failed ? std::string(interp::name_of(o.failure.reason))
                                                                : std::string("a value");
  check(ok, std::format("{}: expected {}, got {}", what, interp::name_of(r), got), at);
}

// ------------------------------------------------------------------ arithmetic

void arithmetic() {
  // int f(int a, int b) { return a + b; }
  Run r(R"(
func {@f} {
^0(%0: !state, %1: {int}, %2: {int}):
  %3: {int} = add nsw %1, %2
  ret %0, %3
}
)");
  expect_value(r.go({i(2), i(40)}), 42, "2 + 40");
  expect_value(r.go({i(-5), i(3)}), -2, "-5 + 3");
  expect_failure(r.go({i(2147483647), i(1)}), Reason::signed_overflow, "INT_MAX + 1");
  expect_failure(r.go({i(-2147483648), i(-1)}), Reason::signed_overflow, "INT_MIN - 1");
  expect_value(r.runtime({i(2147483646), i(1)}), 2147483647, "INT_MAX - 1 + 1");

  // Unsigned arithmetic wraps ([basic.fundamental]); a conversion to a narrower type is modular.
  Run u(R"(
func {@g} {
^0(%0: !state, %1: {uint}):
  %2: {uint} = const [{u1}]
  %3: {uint} = sub %2, %1
  %4: {char} = iconv %3
  %5: {int} = iconv %4
  %6: {int} = iconv %3
  %7: {int} = const [{i1000}]
  %8: {int} = mul nsw %5, %7
  %9: {int} = add nsw %8, %6
  ret %0, %9
}
)");
  // 1u - 3u = 4294967294; as char -2; as int -2 (conversion modulo 2^32).
  expect_value(u.go({i(3)}), -2 * 1000 + -2, "unsigned wrap and narrowing");

  Run mul(R"(
func {@m} {
^0(%0: !state, %1: {long}, %2: {long}):
  %3: {long} = mul nsw %1, %2
  ret %0, %3
}
)");
  expect_value(mul.go({i(std::int64_t(1) << 31), i(std::int64_t(1) << 31)}), std::int64_t(1) << 62, "2^31 * 2^31 in long");
  expect_failure(mul.go({i(std::int64_t(1) << 32), i(std::int64_t(1) << 31)}), Reason::signed_overflow, "2^32 * 2^31 in long");
  expect_failure(mul.go({i(-1), i(std::numeric_limits<std::int64_t>::min())}), Reason::signed_overflow, "-1 * LONG_MIN");
  expect_value(mul.go({i(-1), i(std::numeric_limits<std::int64_t>::max())}), -std::numeric_limits<std::int64_t>::max(),
               "-1 * LONG_MAX");

  Run neg(R"(
func {@n} {
^0(%0: !state, %1: {int}):
  %2: {int} = neg nsw %1
  ret %0, %2
}
)");
  expect_value(neg.go({i(7)}), -7, "-7");
  expect_failure(neg.go({i(-2147483648)}), Reason::signed_overflow, "-INT_MIN");
}

void division_and_shifts() {
  Run d(R"(
func {@d} {
^0(%0: !state, %1: {int}, %2: {int}):
  %3: {int} = sdiv %1, %2
  %4: {int} = srem %1, %2
  %5: {int} = const [{i100}]
  %6: {int} = mul nsw %3, %5
  %7: {int} = add nsw %6, %4
  ret %0, %7
}
)");
  expect_value(d.go({i(-7), i(2)}), -3 * 100 + -1, "-7 / 2 and -7 % 2 truncate toward zero");
  expect_failure(d.go({i(1), i(0)}), Reason::division_by_zero, "1 / 0");
  expect_failure(d.go({i(-2147483648), i(-1)}), Reason::quotient_overflow, "INT_MIN / -1");

  Run ud(R"(
func {@ud} {
^0(%0: !state, %1: {uint}, %2: {uint}):
  %3: {uint} = urem %1, %2
  ret %0, %3
}
)");
  expect_value(ud.go({i(4294967295), i(10)}), 5, "UINT_MAX % 10");
  expect_failure(ud.go({i(1), i(0)}), Reason::division_by_zero, "1u % 0u");

  Run sh(R"(
func {@sh} {
^0(%0: !state, %1: {int}, %2: {int}):
  %3: {int} = shl %1, %2
  %4: {int} = ashr %1, %2
  %5: {int} = add %3, %4
  ret %0, %5
}
)");
  expect_value(sh.go({i(-8), i(1)}), -16 + -4, "-8 << 1 and -8 >> 1");
  expect_value(sh.go({i(1), i(31)}), -2147483648, "1 << 31 is INT_MIN ([expr.shift], modulo 2^32)");
  expect_failure(sh.go({i(1), i(32)}), Reason::shift_count, "1 << 32");
  expect_failure(sh.go({i(1), i(-1)}), Reason::shift_count, "1 << -1");

  Run lsh(R"(
func {@lsh} {
^0(%0: !state, %1: {uint}, %2: {int}):
  %3: {uint} = lshr %1, %2
  ret %0, %3
}
)");
  expect_value(lsh.go({i(4294967295), i(28)}), 15, "UINT_MAX >> 28");
}

// ------------------------------------------------------------------ control flow and calls

// int sum(int n) { int s = 0; for (int k = 1; k <= n; ++k) s += k; return s; }
constexpr std::string_view sum_loop = R"(
func {@sum} {
^0(%0: !state, %1: {int}):
  %2: {int} = const [{i0}]
  %3: {int} = const [{i1}]
  br ^1(%0, %2, %3)
^1(%4: !state, %5: {int}, %6: {int}):
  %7: {bool} = icmp %6, %1 [sle]
  cond_br %7 ^2(%4), ^3(%4)
^2(%8: !state):
  %9: {int} = add nsw %5, %6
  %10: {int} = const [{i1}]
  %11: {int} = add nsw %6, %10
  br ^1(%8, %9, %11)
^3(%12: !state):
  ret %12, %5
}
)";

void loops_and_limits() {
  Run r(sum_loop);
  expect_value(r.go({i(10)}), 55, "sum(10)");
  expect_value(r.go({i(0)}), 0, "sum(0)");
  expect_failure(r.go({i(100000)}), Reason::signed_overflow, "sum(100000) overflows int");
  auto o = r.go({i(1000)}, {.step_limit = 500});
  expect_failure(o, Reason::step_limit, "a step limit of 500");
  check(o.failure.arg == 500, "the step limit is the message's argument");
  check(interp::message(o.failure) == "the evaluation exceeded its limit of 500 steps", interp::message(o.failure));

  // Cancellation is polled every `poll_interval` steps, and nothing is kept.
  r.host.stop = true;
  auto c = r.go({i(1000)}, {.poll_interval = 16});
  check(c.status == interp::Outcome::Status::cancelled && c.steps == 16, "cancelled at the first poll");
  r.host.stop = false;
  expect_value(r.go({i(1000)}, {.poll_interval = 16}), 500500, "polling without a stop changes nothing");
}

// int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
constexpr std::string_view fact = R"(
func {@fact} {
^0(%0: !state, %1: {int}):
  %2: {int} = const [{i1}]
  %3: {bool} = icmp %1, %2 [sle]
  cond_br %3 ^1(%0), ^2(%0)
^1(%4: !state):
  ret %4, %2
^2(%5: !state):
  %6: {int} = sub nsw %1, %2
  %7: !state, %8: {int} = call %5, %6 [{@fact}, 0, 0, 0]
  %9: {int} = mul nsw %1, %8
  ret %7, %9
}
)";

void recursion() {
  Run r(fact);
  expect_value(r.go({i(10)}), 3628800, "fact(10)");
  check(r.host.body_fetches[std::to_underlying(r.w.entity("fact"))] == 1, "a callee's body is fetched once per evaluation");
  expect_failure(r.go({i(13)}), Reason::signed_overflow, "fact(13) overflows int");
  auto o = r.go({i(10)}, {.depth_limit = 5});
  expect_failure(o, Reason::depth_limit, "a depth limit of 5");
  check(o.failure.stack.size() == 5, "the failure's stack has one site per frame");
  auto calls = std::views::iota(0u, std::uint32_t(r.fns[0].body.nodes.size())) |
               std::views::filter([&](std::uint32_t k) { return r.fns[0].body.nodes[k].op == ir::Op::call; });
  check(o.failure.stack[0].function == r.w.entity("fact") && o.failure.stack[0].node == ir::NodeId(*calls.begin()),
        "the innermost site is the call that went too deep");

  // int loop(int n) { return loop(n); }
  Run inf(R"(
func {@loop} {
^0(%0: !state, %1: {int}):
  %2: !state, %3: {int} = call %0, %1 [{@loop}, 0, 0, 0]
  ret %2, %3
}
)");
  expect_failure(inf.go({i(1)}), Reason::depth_limit, "infinite recursion");
}

void calls_to_other_functions() {
  // f calls g (not constexpr), h (declared, not defined) and k through a pointer.
  constexpr std::string_view text = R"(
func {@f} {
^0(%0: !state, %1: {int}):
  %2: {int} = const [{i0}]
  %3: {bool} = icmp %1, %2 [eq]
  cond_br %3 ^1(%0), ^2(%0)
^1(%4: !state):
  %5: !state, %6: {int} = call not_constexpr %4, %1 [{@g}, 0, 0, 0]
  ret %5, %6
^2(%7: !state):
  %8: {int} = const [{i1}]
  %9: {bool} = icmp %1, %8 [eq]
  cond_br %9 ^3(%7), ^4(%7)
^3(%10: !state):
  %11: !state, %12: {int} = call %10, %1 [{@h}, 0, 0, 0]
  ret %11, %12
^4(%13: !state):
  %14: {int*} = func_addr [{@k}]
  %15: !state, %16: {int} = call_ind %13, %14, %1 [0, 0, 0]
  ret %15, %16
}
func {@g} {
^0(%0: !state, %1: {int}):
  %2: {int} = const [{i7}]
  ret %0, %2
}
func {@k} {
^0(%0: !state, %1: {int}):
  %2: {int} = const [{i9}]
  ret %0, %2
}
)";
  Run r(text, {"g"});
  expect_failure(r.go({i(0)}), Reason::not_constexpr, "a call to a non-constexpr function");
  expect_value(r.runtime({i(0)}), 7, "the same call in runtime mode");
  expect_failure(r.go({i(1)}), Reason::undefined_function, "a call to an undefined function");
  expect_value(r.go({i(2)}), 9, "a call through a function pointer");
  Run plain(text, {"g", "k"});
  expect_failure(plain.go({i(2)}), Reason::not_constexpr, "a call through a pointer to a non-constexpr function");
}

void constant_evaluation_ops() {
  // is_const_eval and consteval_br answer by mode ([expr.const], [stmt.if]).
  Run r(R"(
func {@ce} {
^0(%0: !state):
  %1: {bool} = is_const_eval
  %2: {int} = iconv %1
  consteval_br ^1(%0, %2), ^2(%0, %2)
^1(%3: !state, %4: {int}):
  %5: {int} = const [{i10}]
  %6: {int} = add nsw %4, %5
  ret %3, %6
^2(%7: !state, %8: {int}):
  ret %7, %8
}
)");
  expect_value(r.go(), 11, "constant mode: true and the consteval edge");
  expect_value(r.runtime(), 0, "runtime mode: false and the runtime edge");
}

// ------------------------------------------------------------------ memory and lifetime

void lifetime() {
  // int f(int k) { int x = 5; int* p = &x; ... }: k picks the misuse.
  Run r(R"(
func {@f} {
^0(%0: !state, %1: {int}):
  %2: !state, %3: {int*} = alloc %0 [{int}, automatic, erroneous]
  %4: {int} = const [{i0}]
  %5: {bool} = icmp %1, %4 [eq]
  cond_br %5 ^1(%2), ^2(%2)
^1(%6: !state):
  %7: !state = obj.begin %6, %3 [{int}]
  %8: {int} = load %7, %3 [{int}, /]
  ret %7, %8
^2(%9: !state):
  %10: {int} = const [{i5}]
  %11: !state = store %9, %3, %10 [{int}, /]
  %12: !state = obj.begin %11, %3 [{int}]
  %13: {int} = const [{i1}]
  %14: {bool} = icmp %1, %13 [eq]
  cond_br %14 ^3(%12), ^4(%12)
^3(%15: !state):
  %16: !state = obj.end %15, %3
  %17: {int} = load %16, %3 [{int}, /]
  ret %16, %17
^4(%18: !state):
  %19: {int} = const [{i2}]
  %20: {bool} = icmp %1, %19 [eq]
  cond_br %20 ^5(%18), ^6(%18)
^5(%21: !state):
  %22: !state = obj.end %21, %3
  %23: !state = dealloc %22, %3 [automatic]
  %24: {int} = load %23, %3 [{int}, /]
  ret %23, %24
^6(%25: !state):
  %26: {int} = load %25, %3 [{int}, /]
  ret %25, %26
}
)");
  expect_failure(r.go({i(0)}), Reason::read_erroneous, "a read of an uninitialized variable in constant mode");
  expect_value(r.runtime({i(0)}), 0, "the erroneous value in runtime mode (P2795)");
  expect_failure(r.go({i(1)}), Reason::outside_lifetime, "a read after its lifetime ended");
  expect_failure(r.go({i(2)}), Reason::storage_ended, "a read after its storage ended");
  expect_value(r.go({i(3)}), 5, "a read within its lifetime");

  // An indeterminate read, and a read before the lifetime begins.
  Run ind(R"(
func {@ind} {
^0(%0: !state, %1: {bool}):
  %2: !state, %3: {int*} = alloc %0 [{int}, automatic, indeterminate]
  cond_br %1 ^1(%2), ^2(%2)
^1(%4: !state):
  %5: !state = obj.begin %4, %3 [{int}]
  %6: {int} = load %5, %3 [{int}, /]
  ret %5, %6
^2(%7: !state):
  %8: {int} = load %7, %3 [{int}, /]
  ret %7, %8
}
)");
  expect_failure(ind.go({i(1)}), Reason::read_indeterminate, "an indeterminate read");
  expect_failure(ind.runtime({i(1)}), Reason::read_indeterminate, "an indeterminate read in runtime mode");
  expect_failure(ind.go({i(0)}), Reason::outside_lifetime, "a read before the lifetime begins");

  // int* esc() { int x = 1; return &x; }  int use() { return *esc(); }: a dangling pointer.
  Run dangling(R"(
func {@use} {
^0(%0: !state):
  %1: !state, %2: {int*} = call %0 [{@esc}, 0, 0, 0]
  %3: {int} = load %1, %2 [{int}, /]
  ret %1, %3
}
func {@esc} {
^0(%0: !state):
  %1: !state, %2: {int*} = alloc %0 [{int}, automatic, indeterminate]
  %3: {int} = const [{i1}]
  %4: !state = store %1, %2, %3 [{int}, /]
  %5: !state = obj.begin %4, %2 [{int}]
  %6: !state = obj.end %5, %2
  %7: !state = dealloc %6, %2 [automatic]
  ret %7, %2
}
)");
  expect_failure(dangling.go(), Reason::storage_ended, "a read through a dangling pointer");

  // const int c = 1; then a store to it ([dcl.type.cv]/4).
  Run cst(R"(
func {@c} {
^0(%0: !state):
  %1: !state, %2: {int*} = alloc %0 [{cint}, automatic, indeterminate]
  %3: {int} = const [{i1}]
  %4: !state = store %1, %2, %3 [{int}, /]
  %5: !state = obj.begin %4, %2 [{cint}]
  %6: !state = store %5, %2, %3 [{int}, /]
  ret %6, %3
}
)");
  expect_failure(cst.go(), Reason::modify_const, "a store to a const object after its initialization");
}

void arrays_and_pointers() {
  // int a[3]; a[k] = 7; return a[0] + a[1] + a[2] with the rest zero-initialized.
  Run r(R"(
func {@arr} {
^0(%0: !state, %1: {int}):
  %2: !state, %3: {int[3]*} = alloc %0 [{int[3]}, automatic, zero]
  %4: !state = obj.begin %2, %3 [{int[3]}]
  %5: {int*} = ptr_member %3 [{int}, /e0]
  %6: {int*} = ptr_add %5, %1 [{int}]
  %7: {int} = const [{i7}]
  %8: !state = store %4, %6, %7 [{int}, /]
  %9: {int} = load %8, %3 [{int}, /e0]
  %10: {int} = load %8, %3 [{int}, /e1]
  %11: {int} = load %8, %3 [{int}, /e2]
  %12: {int} = add nsw %9, %10
  %13: {int} = add nsw %12, %11
  ret %8, %13
}
)");
  expect_value(r.go({i(2)}), 7, "a[2] = 7");
  expect_failure(r.go({i(3)}), Reason::past_end_access, "a[3] = 7 stores past the end");
  expect_failure(r.go({i(4)}), Reason::pointer_bounds, "a + 4 leaves the array");
  expect_failure(r.go({i(-1)}), Reason::pointer_bounds, "a - 1 leaves the array");

  // Differences and comparisons: k picks the operation.
  Run p(R"(
func {@p} {
^0(%0: !state, %1: {int}):
  %2: !state, %3: {int[3]*} = alloc %0 [{int[3]}, automatic, zero]
  %4: !state, %5: {int*} = alloc %2 [{int}, automatic, zero]
  %6: {int*} = ptr_member %3 [{int}, /e0]
  %7: {int} = const [{i3}]
  %8: {int*} = ptr_add %6, %7 [{int}]
  %9: {int} = const [{i0}]
  %10: {bool} = icmp %1, %9 [eq]
  cond_br %10 ^1(%4), ^2(%4)
^1(%11: !state):
  %12: {long} = ptr_diff %8, %6 [{int}]
  %13: {int} = iconv %12
  ret %11, %13
^2(%14: !state):
  %15: {int} = const [{i1}]
  %16: {bool} = icmp %1, %15 [eq]
  cond_br %16 ^3(%14), ^4(%14)
^3(%17: !state):
  %18: {bool} = ptr_cmp %6, %8 [lt]
  %19: {int} = iconv %18
  ret %17, %19
^4(%20: !state):
  %21: {int} = const [{i2}]
  %22: {bool} = icmp %1, %21 [eq]
  cond_br %22 ^5(%20), ^6(%20)
^5(%23: !state):
  %24: {bool} = ptr_cmp %6, %5 [eq]
  %25: {int} = iconv %24
  ret %23, %25
^6(%26: !state):
  %27: {int} = const [{i3}]
  %28: {bool} = icmp %1, %27 [eq]
  cond_br %28 ^7(%26), ^8(%26)
^7(%29: !state):
  %30: {bool} = ptr_cmp %6, %5 [lt]
  %31: {int} = iconv %30
  ret %29, %31
^8(%32: !state):
  %33: {int} = const [{i4}]
  %34: {bool} = icmp %1, %33 [eq]
  cond_br %34 ^9(%32), ^10(%32)
^9(%35: !state):
  %36: {bool} = ptr_cmp %8, %5 [eq]
  %37: {int} = iconv %36
  ret %35, %37
^10(%38: !state):
  %39: {long} = ptr_diff %8, %5 [{int}]
  %40: {int} = iconv %39
  ret %38, %40
}
)");
  expect_value(p.go({i(0)}), 3, "(a + 3) - a");
  expect_value(p.go({i(1)}), 1, "a < a + 3");
  expect_value(p.go({i(2)}), 0, "a == &x is false");
  expect_failure(p.go({i(3)}), Reason::unspecified_comparison, "a < &x is unspecified");
  expect_failure(p.go({i(4)}), Reason::unspecified_comparison, "a + 3 == &x is unspecified");
  expect_failure(p.go({i(5)}), Reason::pointer_difference, "(a + 3) - &x");

  // A pointer to a non-array object is an array of one ([expr.add]/1): p + 1 is past it, and
  // (p + 1) - 1 is p again.
  Run one(R"(
func {@one} {
^0(%0: !state, %1: {int}):
  %2: !state, %3: {int*} = alloc %0 [{int}, automatic, zero]
  %4: {int*} = ptr_add %3, %1 [{int}]
  %5: {int} = const [{i0}]
  %6: {int} = sub nsw %5, %1
  %7: {int*} = ptr_add %4, %6 [{int}]
  %8: !state = obj.begin %2, %3 [{int}]
  %9: {int} = load %8, %7 [{int}, /]
  %10: {int} = load %8, %4 [{int}, /]
  ret %8, %10
}
)");
  expect_value(one.go({i(0)}), 0, "&x + 0");
  expect_failure(one.go({i(1)}), Reason::past_end_access, "a read of *(&x + 1)");
  expect_failure(one.go({i(2)}), Reason::pointer_bounds, "&x + 2");

  // Two-dimensional: int m[2][3]; m[1][2] through the rows.
  Run m(R"(
func {@m} {
^0(%0: !state):
  %1: !state, %2: {int[2][3]*} = alloc %0 [{int[2][3]}, automatic, zero]
  %3: !state = obj.begin %1, %2 [{int[2][3]}]
  %4: {int[3]*} = ptr_member %2 [{int[3]}, /e0]
  %5: {int} = const [{i1}]
  %6: {int[3]*} = ptr_add %4, %5 [{int[3]}]
  %7: {int*} = ptr_member %6 [{int}, /e0]
  %8: {int} = const [{i2}]
  %9: {int*} = ptr_add %7, %8 [{int}]
  %10: {int} = const [{i42}]
  %11: !state = store %3, %9, %10 [{int}, /]
  %12: {int} = load %11, %2 [{int}, /e1/e2]
  ret %11, %12
}
)");
  expect_value(m.go(), 42, "m[1][2]");

  // A type mismatch in pointer arithmetic, and a null pointer.
  Run bad(R"(
func {@bad} {
^0(%0: !state, %1: {bool}):
  %2: !state, %3: {int[3]*} = alloc %0 [{int[3]}, automatic, zero]
  %4: {int} = const [{i1}]
  cond_br %1 ^1(%2), ^2(%2)
^1(%5: !state):
  %6: {int*} = ptr_add %3, %4 [{int}]
  ret %5, %4
^2(%7: !state):
  %8: {int*} = const [{null}]
  %9: {int} = load %7, %8 [{int}, /]
  ret %7, %9
}
)");
  expect_failure(bad.go({i(1)}), Reason::pointer_type, "an int[3]* stepped as int*");
  expect_failure(bad.go({i(0)}), Reason::null_access, "a read through a null pointer");
}

void string_literals() {
  // "hi"[k], and a store into the literal.
  Run r(R"(
func {@lit} {
^0(%0: !state, %1: {int}):
  %2: {cchar[3]*} = const [{s}]
  %3: {cchar*} = ptr_member %2 [{cchar}, /e0]
  %4: {cchar*} = ptr_add %3, %1 [{cchar}]
  %5: {char} = load %0, %4 [{char}, /]
  %6: {int} = iconv %5
  ret %0, %6
}
func {@wr} {
^0(%0: !state):
  %1: {cchar[3]*} = const [{s}]
  %2: {char} = const [{i0}]
  %3: !state = store %0, %1, %2 [{char}, /e0]
  ret %3, %2
}
)");
  expect_value(r.go({i(1)}), 'i', "\"hi\"[1]");
  expect_value(r.go({i(2)}), 0, "\"hi\"[2]");
  expect_failure(r.go({i(3)}), Reason::past_end_access, "\"hi\"[3]");
  auto wr = interp::evaluate(r.host, r.w.in, target, r.fns[1].body, r.fns[1].name, {}).run();
  expect_failure(wr, Reason::modify_const, "a store into a string literal");

  // A string literal's address is a constant result.
  Run res(R"(
func {@res} {
^0(%0: !state):
  %1: {cchar[3]*} = const [{s}]
  %2: {cchar*} = ptr_member %1 [{cchar}, /e0]
  %3: {int} = const [{i1}]
  %4: {cchar*} = ptr_add %2, %3 [{cchar}]
  ret %0, %4
}
)");
  auto o = res.go();
  check(o.status == interp::Outcome::Status::value && o.values.size() == 1 &&
            o.values[0].kind == interp::Result::Kind::address && o.values[0].base == interp::Base::literal &&
            o.values[0].literal == res.w.hi && o.values[0].path == std::vector{ir::PathStep{ir::Step::elem, 1}},
        "\"hi\" + 1 is an address within the literal");
}

void globals() {
  // const int n = 4; int g;  k picks: read n, read g, write g, address of g.
  Run r(R"(
func {@f} {
^0(%0: !state, %1: {int}):
  %2: {int*} = global [{@n}]
  %3: {int*} = global [{@g}]
  %4: {int} = const [{i0}]
  %5: {bool} = icmp %1, %4 [eq]
  cond_br %5 ^1(%0), ^2(%0)
^1(%6: !state):
  %7: {int} = load %6, %2 [{int}, /]
  ret %6, %7
^2(%8: !state):
  %9: {int} = const [{i1}]
  %10: {bool} = icmp %1, %9 [eq]
  cond_br %10 ^3(%8), ^4(%8)
^3(%11: !state):
  %12: {int} = load %11, %3 [{int}, /]
  ret %11, %12
^4(%13: !state):
  %14: {int} = const [{i3}]
  %15: !state = store %13, %3, %14 [{int}, /]
  %16: {int} = load %15, %3 [{int}, /]
  ret %15, %16
}
)");
  r.host.globals[std::to_underlying(r.w.entity("n"))] = {r.w.cint_t, ast::int_value(r.w.in, target, r.w.int_t, 4), true};
  r.host.globals[std::to_underlying(r.w.entity("g"))] = {r.w.int_t, std::nullopt, false};
  expect_value(r.go({i(0)}), 4, "a read of a variable usable in constant expressions");
  expect_failure(r.go({i(1)}), Reason::not_usable, "a read of a variable that is not");
  expect_failure(r.go({i(2)}), Reason::modify_outside, "a store to a global in constant mode");
  expect_value(r.runtime({i(1)}), 0, "a global is zero-initialized in runtime mode");
  expect_value(r.runtime({i(2)}), 3, "and modifiable");
}

void unsequenced() {
  // int u(int x) { return x + (x = 1); }, as function_ir generates it (#24).
  Run r(R"(
func {@u} {
^0(%0: !state, %1: {int}):
  %2: !state, %3: {int*} = alloc %0 [{int}, automatic, indeterminate]
  %4: !state = store %2, %3, %1 [{int}, /]
  %5: !state = obj.begin %4, %3 [{int}]
  %6: !state = useq.begin %5
  %7: {int} = load %6, %3 [{int}, /]
  %8: !state = useq.next %6
  %9: {int} = const [{i1}]
  %10: !state = store %8, %3, %9 [{int}, /]
  %11: {int} = load %10, %3 [{int}, /]
  %12: !state = useq.end %10
  %13: !state = obj.end %12, %3
  %14: !state = dealloc %13, %3 [automatic]
  %15: {int} = add nsw %7, %11
  ret %14, %15
}
)");
  expect_failure(r.go({i(5)}), Reason::unsequenced, "x + (x = 1)");
  expect_failure(r.runtime({i(5)}), Reason::unsequenced, "x + (x = 1) in runtime mode");
}

void unsupported_and_nonconstant() {
  Run r(R"(
func {@f} {
^0(%0: !state, %1: {bool}):
  %2: !state, %3: {int*} = alloc %0 [{int}, automatic, zero]
  cond_br %1 ^1(%2), ^2(%2)
^1(%4: !state):
  %5: {long} = ptr_to_int %3
  %6: {int} = iconv %5
  ret %4, %6
^2(%7: !state):
  %8: {int*} = vtable_addr [{@f}]
  ret %7, %8
}
)");
  auto o = r.go({i(1)});
  expect_failure(o, Reason::reinterpret, "a pointer converted to an integer");
  auto u = r.go({i(0)});
  expect_failure(u, Reason::unsupported, "an op the α slice does not evaluate");
  check(interp::message(u.failure) == "the interpreter does not evaluate 'vtable_addr' yet", interp::message(u.failure));
}

// Regressions from the review of #25's first commit.
void review_regressions() {
  // a + LONG_MIN leaves the array; negating the index must not overflow.
  Run r(R"(
func {@f} {
^0(%0: !state, %1: {long}):
  %2: !state, %3: {int[3]*} = alloc %0 [{int[3]}, automatic, zero]
  %4: !state = obj.begin %2, %3 [{int[3]}]
  %5: {int*} = ptr_member %3 [{int}, /e0]
  %6: {int*} = ptr_add %5, %1 [{int}]
  %7: {int} = load %4, %6 [{int}, /]
  ret %4, %7
}
)");
  expect_failure(r.go({i(std::numeric_limits<std::int64_t>::min())}), Reason::pointer_bounds, "a + LONG_MIN");

  // An integral conversion to bool is the boolean conversion: 2 becomes true.
  Run b(R"(
func {@b} {
^0(%0: !state, %1: {int}):
  %2: {bool} = iconv %1
  %3: {int} = iconv %2
  ret %0, %3
}
)");
  expect_value(b.go({i(2)}), 1, "bool(2)");

  // Pointers compare by address: &m[0][3] == &m[1][0]; &m[0][3] is not past m, so it is unequal
  // to &x; two string literals may overlap.
  Run c(R"(
func {@c} {
^0(%0: !state, %1: {int}):
  %2: !state, %3: {int[2][3]*} = alloc %0 [{int[2][3]}, automatic, zero]
  %4: !state, %5: {int*} = alloc %2 [{int}, automatic, zero]
  %6: {int*} = ptr_member %3 [{int}, /e0/e0]
  %7: {int} = const [{i3}]
  %8: {int*} = ptr_add %6, %7 [{int}]
  %9: {int*} = ptr_member %3 [{int}, /e1/e0]
  %10: {int} = const [{i0}]
  %11: {bool} = icmp %1, %10 [eq]
  cond_br %11 ^1(%4), ^2(%4)
^1(%12: !state):
  %13: {bool} = ptr_cmp %8, %9 [eq]
  %14: {int} = iconv %13
  ret %12, %14
^2(%15: !state):
  %16: {int} = const [{i1}]
  %17: {bool} = icmp %1, %16 [eq]
  cond_br %17 ^3(%15), ^4(%15)
^3(%18: !state):
  %19: {bool} = ptr_cmp %8, %5 [eq]
  %20: {int} = iconv %19
  ret %18, %20
^4(%21: !state):
  %22: {cchar[3]*} = const [{s}]
  %23: {cchar[3]*} = const [{s}]
  %24: {bool} = ptr_cmp %22, %23 [eq]
  %25: {int} = iconv %24
  ret %21, %25
}
)");
  expect_value(c.go({i(0)}), 1, "&m[0][3] == &m[1][0]");
  expect_value(c.go({i(1)}), 0, "&m[0][3] == &x");
  expect_failure(c.go({i(2)}), Reason::unspecified_comparison, "\"hi\" == \"hi\"");

  // Ending the lifetime of a global from a constant evaluation modifies it.
  Run e(R"(
func {@e} {
^0(%0: !state):
  %1: {int*} = global [{@g}]
  %2: !state = obj.end %0, %1
  %3: {int} = const [{i0}]
  ret %2, %3
}
)");
  e.host.globals[std::to_underlying(e.w.entity("g"))] = {e.w.int_t, std::nullopt, false};
  expect_failure(e.go(), Reason::modify_outside, "obj.end of a global");

  // A zero poll interval never polls.
  Run l(sum_loop);
  expect_value(l.go({i(10)}, {.poll_interval = 0}), 55, "poll_interval 0");

  // Runtime mode reads an int through an unsigned glvalue ([basic.lval]/11); constant mode does not.
  Run u(R"(
func {@u} {
^0(%0: !state):
  %1: !state, %2: {int*} = alloc %0 [{int}, automatic, indeterminate]
  %3: {int} = const [{i-1}]
  %4: !state = store %1, %2, %3 [{int}, /]
  %5: !state = obj.begin %4, %2 [{int}]
  %6: {uint} = load %5, %2 [{uint}, /]
  %7: {int} = iconv %6
  ret %5, %7
}
)");
  expect_value(u.runtime(), -1, "int read as unsigned in runtime mode");
  expect_failure(u.go(), Reason::access_type, "int read as unsigned in constant mode");

  // static_cast<int*>(void* to a char) is not constant ([expr.const]); int is QualType 0 here,
  // which must not read as "no pointee".
  Run vc(R"(
func {@vc} {
^0(%0: !state):
  %1: !state, %2: {char*} = alloc %0 [{char}, automatic, zero]
  %3: {void*} = ptr_cast %2 [{void*}]
  %4: {int*} = ptr_cast %3 [{int*}]
  %5: {int} = const [{i0}]
  ret %1, %5
}
)");
  check(std::to_underlying(vc.w.int_t) == 0, "the test's int is QualType 0");
  expect_failure(vc.go(), Reason::void_cast_type, "void* to a char converted to int*");

  // Only the signed or unsigned counterpart: not long through long long, nor int through wchar_t.
  Run w(R"(
func {@w} {
^0(%0: !state):
  %1: !state, %2: {long*} = alloc %0 [{long}, automatic, zero]
  %3: !state = obj.begin %1, %2 [{long}]
  %4: {llong} = load %3, %2 [{llong}, /]
  %5: {int} = iconv %4
  ret %3, %5
}
)");
  expect_failure(w.runtime(), Reason::access_type, "long read as long long");

  // Literals that cannot share an offset compare unequal ([expr.const]): "hi" + 1 and "xy" + 1,
  // and "hi" + 1 and "ci" + 1 too, since 'h' and 'c' precede them. ("hi" == "hi" is unspecified,
  // above.)
  Run lit(R"(
func {@lit} {
^0(%0: !state, %1: {bool}):
  %2: {cchar[3]*} = const [{s}]
  %3: {cchar*} = ptr_member %2 [{cchar}, /e0]
  %4: {int} = const [{i1}]
  %5: {cchar*} = ptr_add %3, %4 [{cchar}]
  cond_br %1 ^1(%0), ^2(%0)
^1(%6: !state):
  %7: {cchar[3]*} = const [{s2}]
  %8: {cchar*} = ptr_member %7 [{cchar}, /e0]
  %9: {cchar*} = ptr_add %8, %4 [{cchar}]
  %10: {bool} = ptr_cmp %5, %9 [eq]
  %11: {int} = iconv %10
  ret %6, %11
^2(%12: !state):
  %13: {cchar[3]*} = const [{s3}]
  %14: {cchar*} = ptr_member %13 [{cchar}, /e0]
  %15: {cchar*} = ptr_add %14, %4 [{cchar}]
  %16: {bool} = ptr_cmp %5, %15 [eq]
  %17: {int} = iconv %16
  ret %12, %17
}
)");
  expect_value(lit.go({i(1)}), 0, "\"hi\" + 1 == \"xy\" + 1");
  expect_value(lit.go({i(0)}), 0, "\"hi\" + 1 == \"ci\" + 1");
}

// Address constants of entities (#57): p = &a[1] is decoded when p is first read, a function's
// address is a function pointer, and an address result carries its byte offset.
void entity_addresses() {
  auto setup = [](World& w) {
    ast::Interners& in = w.in;
    auto addr = [&](const std::string& e, std::int64_t off) {
      ConstId base = in.intern_value(ast::VEntity{w.entity(e)});
      return in.intern_value(ast::VLvalue{base, in.intern_seq(std::span<const std::uint32_t>{}), off});
    };
    w.keys["{&a1}"] = std::format("#{}", std::to_underlying(addr("a", 4)));
    w.keys["{&k}"] = std::format("#{}", std::to_underlying(addr("k", 0)));
  };
  Run r(R"(
func {@f} {
^0(%0: !state, %1: {int}):
  %2: {int**} = global [{@p}]
  %3: {int*} = load %0, %2 [{int*}, /]
  %4: {int} = const [{i0}]
  %5: {bool} = icmp %1, %4 [eq]
  cond_br %5 ^1(%0), ^2(%0)
^1(%6: !state):
  %7: {int} = load %6, %3 [{int}, /]
  ret %6, %7
^2(%8: !state):
  %9: {int(*)(int)} = const [{&k}]
  %10: !state, %11: {int} = call_ind %8, %9, %1 [0, 0, 0]
  ret %10, %11
}
func {@k} {
^0(%0: !state, %1: {int}):
  ret %0, %1
}
func {@res} {
^0(%0: !state):
  %1: {int*} = const [{&a1}]
  %2: {int} = const [{i1}]
  %3: {int*} = ptr_add %1, %2 [{int}]
  ret %0, %3
}
)", {}, setup);
  ast::Interners& in = r.w.in;
  QualType int3 = ast::array_of(in, target, r.w.int_t, ast::int_value(in, target, ast::Builtin::unsigned_long, 3));
  r.host.globals[std::to_underlying(r.w.entity("a"))] = {int3, std::nullopt, false};
  ConstId p_value = in.intern_value(ast::VLvalue{in.intern_value(ast::VEntity{r.w.entity("a")}),
                                                 in.intern_seq(std::span<const std::uint32_t>{}), 4});
  r.host.globals[std::to_underlying(r.w.entity("p"))] = {ast::pointer_to(in, r.w.int_t), p_value, true};
  expect_value(r.runtime({i(0)}), 0, "*p with p = &a[1], a zero-initialized");
  expect_failure(r.go({i(0)}), Reason::not_usable, "*p reads a, which is not usable in constant expressions");
  expect_value(r.go({i(5)}), 5, "a call through the constant address of k");
  auto o = interp::evaluate(r.host, in, target, r.fns[2].body, r.fns[2].name, {}).run();
  check(o.status == interp::Outcome::Status::value && o.values.size() == 1 &&
            o.values[0].kind == interp::Result::Kind::address && o.values[0].base == interp::Base::global &&
            o.values[0].entity == r.w.entity("a") && o.values[0].offset == 8,
        "&a[1] + 1 is the address of a plus 8 bytes");
}

}  // namespace

int main() {
  entity_addresses();
  review_regressions();
  arithmetic();
  division_and_shifts();
  loops_and_limits();
  recursion();
  calls_to_other_functions();
  constant_evaluation_ops();
  lifetime();
  arrays_and_pointers();
  string_literals();
  globals();
  unsequenced();
  unsupported_and_nonconstant();
  return ycxx::unit::result("unit.interp");
}
