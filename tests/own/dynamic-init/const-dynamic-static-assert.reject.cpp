// ι (#94), spec-only: a `const int` whose initializer is not a constant expression is dynamically
// initialized, so it is not usable in constant expressions and cannot be the condition of a
// static_assert ([expr.const], [basic.start.dynamic], [dcl.pre]).
int f() { return 1; }

const int n = f();
const int m = n + 1;

static_assert(n == 1); //! error
static_assert(m == 2); //! error

int main() { return n - 1; }
