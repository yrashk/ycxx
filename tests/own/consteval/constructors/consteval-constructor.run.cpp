// #102, [expr.const]: immediate invocations of consteval constructors are replaced by their
// values, so the objects they initialize hold those values at run time, and the constructors
// themselves have no code: `never_defined` is never referenced by the program.
void never_defined();
struct Point {
  consteval Point(int a, int b) : x(a), y(b) {
    if (a < 0) never_defined();
  }
  int x;
  int y;
};
int sum(Point p) { return p.x + p.y; }
Point global(10, 20);

struct Doubled {
  consteval Doubled(int);
  int value;
};
consteval Doubled::Doubled(int v) : value(v * 2) {
  if (v < 0) never_defined();
}

// libycxx's comparison-category argument (deps/libycxx/include/ycxx/core/compare.hpp).
struct literal_zero {
  consteval literal_zero(int v) noexcept {
    if (v != 0) never_defined();
  }
};
struct ordering {
  int value;
};
bool operator==(ordering o, literal_zero) { return o.value == 0; }

int main() {
  Point local(3, 4);
  if (sum(local) != 7) return 1;
  if (sum({1, 2}) != 3) return 2;
  if (sum(Point(5, 6)) != 11) return 3;
  if (sum(global) != 30) return 4;
  if (Doubled(21).value != 42) return 5;
  if (!(ordering{0} == 0) || ordering{1} == 0) return 6;
  return 0;
}
