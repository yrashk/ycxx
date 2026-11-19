// ι (#300): constexpr functions, member functions and constructors with default arguments,
// evaluated in constant expressions; a default naming a constexpr variable and calling another
// constexpr function ([dcl.fct.default]/1, [dcl.constexpr], [expr.const]).
// EXIT: 0
constexpr int factor = 10;
constexpr int square(int x) { return x * x; }

constexpr int scale(int x, int k = factor) { return x * k; }
constexpr int offset(int x = square(3), int y = 1) { return x + y; }

struct P {
  int x, y;
  constexpr P(int x = 1, int y = 2) : x(x), y(y) {}
  constexpr int dot(int a = 3, int b = 4) const { return x * a + y * b; }
};

static_assert(scale(2) == 20);
static_assert(scale(2, 3) == 6);
static_assert(offset() == 10);
static_assert(offset(1) == 2);
static_assert(offset(1, 1) == 2);
static_assert(P().y == 2);
static_assert(P(5).x == 5 && P(5).y == 2);
static_assert(P().dot() == 11);
static_assert(P(2, 2).dot(1) == 10);

constexpr P origin{};
static_assert(origin.x == 1);

int buffer[scale(1, 4)];  // a default-using call can also be an array bound
static_assert(sizeof(buffer) == 4 * sizeof(int));

int main() {
  constexpr int r = scale(3);
  if (r != 30) return 1;
  if (offset() != 10) return 2;
  return P(1, 1).dot() == 7 ? 0 : 3;
}
