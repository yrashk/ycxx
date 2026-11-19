// ι (#300): what a default argument can be: a constant expression, an enumerator, a call to
// another function, a global variable. A default argument is evaluated each time the function is
// called without that argument, so it reads the global's current value, and a call that
// supplies the argument does not evaluate the default ([dcl.fct.default]/1, /9, [expr.call]/5).
// EXIT: 0
enum Color { red = 1, green = 2, blue = 4 };
enum class Size { small = 10, large = 20 };

int base = 5;
int calls = 0;
int next() { return ++calls; }
int twice(int x) { return 2 * x; }

int constant(int x = 3 * 4 + 1) { return x; }
int color(Color c = green) { return c; }
int size(Size s = Size::large) { return static_cast<int>(s); }
int called(int x = twice(21)) { return x; }
int global(int x = base) { return x; }
int global_expr(int x = base * 2 + 1) { return x; }
int counted(int x = next()) { return x; }

int main() {
  if (constant() != 13) return 1;
  if (color() != 2 || color(blue) != 4) return 2;
  if (size() != 20 || size(Size::small) != 10) return 3;
  if (called() != 42) return 4;
  if (global() != 5) return 5;
  base = 9;
  if (global() != 9) return 6;            // read at the call, not at the declaration
  if (global_expr() != 19) return 7;
  base = -1;
  if (global_expr() != -1) return 8;
  if (counted() != 1 || counted() != 2) return 9;
  if (counted(100) != 100) return 10;     // a supplied argument: the default is not evaluated
  if (calls != 2) return 11;
  if (counted() != 3 || calls != 3) return 12;
  return 0;
}
