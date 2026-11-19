// #337 (ι): a switch on a scoped enumeration in a constexpr function, evaluated during constant
// evaluation ([dcl.constexpr], [expr.const], [stmt.switch]).
// XFAIL: ycxx the interpreter has no enumeration-typed values yet (#25)
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

static_assert(apply(Op::Add, 6, 7) == 13);
static_assert(apply(Op::Sub, 6, 7) == -1);
static_assert(apply(Op::Mul, 6, 7) == 42);
static_assert(apply(Op::Nop, 6, 7) == 6);

int main() {
  Op op = Op::Mul;
  return apply(op, 3, 4) == 12 ? 0 : 1;
}
