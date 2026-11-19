// ι (#94), spec-only: a `constexpr` variable's initializer must be a constant expression, so it
// cannot call a function that is not constexpr, nor a constructor that is not
// ([dcl.constexpr], [expr.const]).
int f() { return 1; }

struct S {
  int v;
  S(int x) : v(x) {}
};

constexpr int x = f(); //! error
constexpr S s(2); //! error
int fine = f();

int main() { return fine - 1; }
