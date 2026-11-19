// ι (#300): constexpr member functions and constructors with default arguments, evaluated in
// constant expressions ([dcl.fct.default]/1, [dcl.constexpr], [expr.const]).
// XFAIL: ycxx constexpr constructors and class objects in constant evaluation (#102)
// EXIT: 0
struct P {
  int x, y;
  constexpr P(int x = 1, int y = 2) : x(x), y(y) {}
  constexpr int dot(int a = 3, int b = 4) const { return x * a + y * b; }
};

static_assert(P().y == 2);
static_assert(P(5).x == 5 && P(5).y == 2);
static_assert(P().dot() == 11);
static_assert(P(2, 2).dot(1) == 10);
constexpr P origin{};
static_assert(origin.x == 1);

int main() { return P(1, 1).dot() == 7 ? 0 : 3; }
