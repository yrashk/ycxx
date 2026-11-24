// #102, [dcl.constexpr], [expr.const], [class.ctor]: a constructor declared consteval is an
// immediate function. A call of it outside an immediate function context, including one an
// implicit conversion selects, is an immediate invocation: a constant expression.
struct Z {
  consteval Z(int v) : value(v) {}
  int value;
};
constexpr Z z(0);
static_assert(z.value == 0);
constexpr Z braced{4};
static_assert(braced.value == 4);
static_assert(Z(3).value == 3);
static_assert(Z{9}.value == 9);
Z runtime_object(5);           // the immediate invocation is constant; the object is not
int read_local() {
  Z local(6);
  return local.value;
}

// The consteval specifier among the other constructor specifiers, in either order.
struct Ordered {
  explicit consteval Ordered(int v) : value(v) {}
  consteval explicit Ordered(int a, int b) : value(a + b) {}
  int value;
};
static_assert(Ordered(2).value == 2);
static_assert(Ordered(2, 3).value == 5);

// Declared in the class and defined outside it.
struct Out {
  consteval Out(int);
  int v;
};
consteval Out::Out(int x) : v(x * 2) {}
static_assert(Out(4).v == 8);

// libycxx's comparison-category argument (deps/libycxx/include/ycxx/core/compare.hpp): the
// conversion of `0` is a constant expression, so the call of a non-constexpr function on the
// other branch is never evaluated.
void bad();
struct literal_zero {
  consteval literal_zero(int v) noexcept {
    if (v != 0) bad();
  }
};
void f(literal_zero) {}
void g() { f(0); }
struct ordering {
  int value;
};
bool operator==(ordering, literal_zero) { return true; }
bool compare(ordering x) { return x == 0; }

// Inside an immediate function the call is not an immediate invocation, so its arguments need
// not be constant.
consteval int twice(int n) {
  Z local(n);
  return local.value * 2;
}
static_assert(twice(5) == 10);
