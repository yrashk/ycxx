// ι (#300): constexpr functions with default arguments (constructors and member functions are in
// constexpr-class.run.cpp),
// evaluated in constant expressions; a default naming a constexpr variable and calling another
// constexpr function ([dcl.fct.default]/1, [dcl.constexpr], [expr.const]).
// EXIT: 0
constexpr int factor = 10;
constexpr int square(int x) { return x * x; }

constexpr int scale(int x, int k = factor) { return x * k; }
constexpr int offset(int x = square(3), int y = 1) { return x + y; }


static_assert(scale(2) == 20);
static_assert(scale(2, 3) == 6);
static_assert(offset() == 10);
static_assert(offset(1) == 2);
static_assert(offset(1, 1) == 2);


int buffer[scale(1, 4)];  // a default-using call can also be an array bound
static_assert(sizeof(buffer) == 4 * sizeof(int));

int main() {
  constexpr int r = scale(3);
  if (r != 30) return 1;
  if (offset() != 10) return 2;
}
