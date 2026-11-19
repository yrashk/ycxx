// ι (#94), spec-only: a `constinit` variable must have constant initialization, so an initializer
// that calls a non-constexpr function or constructor, or reads a dynamically initialized variable,
// is ill-formed ([dcl.constinit], [basic.start.static], [expr.const]).
int f() { return 1; }

struct S {
  int v;
  S(int x) : v(x) {}
};

int dynamic = f();

constinit int y = f(); //! error
constinit S s(2); //! error
constinit int z = dynamic; //! error

int main() { return dynamic - 1; }
