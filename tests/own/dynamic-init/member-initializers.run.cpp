// ι (#94), spec-only: a namespace-scope object of a class with no user-provided constructor but a
// default member initializer that calls an ordinary function is dynamically initialized, and so is
// an object whose constructor reads an earlier namespace-scope variable; both happen before main,
// in definition order ([basic.start.dynamic], [class.base.init], [dcl.init.general]).
// XFAIL: ycxx a default member initializer that is not a constant, in an aggregate (#118)
// STDOUT: 123M
// EXIT: 0
extern "C" int putchar(int);

int count;

int next() {
  ++count;
  putchar('0' + count);
  return count;
}

struct Defaults {
  int a = next();
  int b = 7;
  int c = a * 10;
};

int scale = next() * 100;

struct Scaled {
  int v;
  Scaled(int x) : v(x * scale) {}
};

Defaults defaults;
Scaled scaled(3);
Defaults braced{};

int main() {
  putchar('M');
  putchar('\n');
  if (scale != 100) return 1;
  if (defaults.a != 2 || defaults.b != 7 || defaults.c != 20) return 2;
  if (scaled.v != 300) return 3;
  if (braced.a != 3 || braced.c != 30) return 4;
  return 0;
}
