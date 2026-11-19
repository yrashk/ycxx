// #337 (ι): a switch statement in a constexpr function, evaluated during constant evaluation
// ([dcl.constexpr], [expr.const]), with fallthrough, default, a loop around it and continue.
// EXIT: 0
enum class Op { Add, Sub, Mul, Nop };

constexpr int apply(Op op, int a, int b) {
  switch (op) {
    case Op::Add: return a + b;
    case Op::Sub: return a - b;
    case Op::Mul: return a * b;
    default: break;
  }
  return a;
}

constexpr int score(int n) {
  int total = 0;
  for (int i = 0; i < n; ++i) {
    switch (i % 4) {
      case 0: total += 1;
      case 1: total += 10; break;
      case 2: continue;
      default: total += 100;
    }
    total += 1000;
  }
  return total;
}

static_assert(apply(Op::Add, 6, 7) == 13);
static_assert(apply(Op::Sub, 6, 7) == -1);
static_assert(apply(Op::Mul, 6, 7) == 42);
static_assert(apply(Op::Nop, 6, 7) == 6);
// i=0: 11+1000, 1: 10+1000, 2: skipped, 3: 100+1000, 4: 11+1000
static_assert(score(5) == 4132);
static_assert(score(0) == 0);

constexpr int at_compile_time = score(8);

int main() {
  if (at_compile_time != 2 * 3121) return 1;
  Op op = Op::Mul;
  if (apply(op, 3, 4) != 12) return 2;
  return 0;
}
