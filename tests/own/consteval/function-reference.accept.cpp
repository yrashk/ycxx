// #102, [expr.const.imm], [dcl.type.decltype]: immediate calls and unevaluated operands.
consteval int immediate(int x) { return x + 1; }

int direct() { return immediate(41); }
int parenthesized() { return (immediate)(41); }

using Signature = decltype(immediate);
using Pointer = decltype(&immediate);
using Result = decltype(immediate(0));
extern int runtime;
using UnevaluatedResult = decltype((immediate)(runtime));

int evaluate(Signature*, Pointer, Result, UnevaluatedResult);
static_assert(immediate(41) == 42);
static_assert((immediate)(41) == 42);

constexpr int call_with_pointer(int (*function)(int), int x) { return function(x); }
static_assert(call_with_pointer(immediate, 41) == 42);

// [expr.const.defns]: the initializer is manifestly constant-evaluated if it
// has constant initialization or initializes a variable usable in constant expressions.
int namespace_value = call_with_pointer(immediate, 41);

int local_value() {
  const int n = call_with_pointer(immediate, 41);
  static_assert(n == 42);
  return n;
}
