// ι (#94), spec-only: constant initialization happens before any dynamic initialization
// ([basic.start.static]), so a dynamic initializer sees the value of a constant-initialized
// variable even when that variable is defined later in the translation unit; a `constexpr` variable
// and a `constinit` one feed dynamic initializers; a `const int` with a dynamic initializer is
// initialized at run time and read like any other variable ([basic.start.dynamic],
// [dcl.constexpr], [dcl.constinit], [expr.const]).
// EXIT: 0
int calls;

int f(int x) {
  ++calls;
  return x;
}

int read_later();

constexpr int base = 40;
int dynamic_sum = f(base) + 2;
int sees_later = read_later();
const int dynamic_const = f(dynamic_sum) * 2;
constinit int counter = base / 4;
int bumped = ++counter;
int constant_later = 6 * 7;
const int constant_const = 5;
int arr[constant_const] = {f(1), f(2)};

int read_later() { return constant_later; }

int main() {
  static_assert(base == 40);
  static_assert(constant_const == 5);
  if (dynamic_sum != 42) return 1;
  if (sees_later != 42) return 2;
  if (dynamic_const != 84) return 3;
  if (counter != 11) return 4;
  if (bumped != 11) return 5;
  if (arr[0] != 1 || arr[1] != 2 || arr[4] != 0) return 6;
  if (calls != 4) return 7;
  return 0;
}
