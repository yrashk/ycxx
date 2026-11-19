// ι (#94), spec-only: a namespace-scope variable whose initializer calls an ordinary function is
// dynamically initialized before main, and the function's side effects are visible in main
// ([basic.start.dynamic], [basic.start.static], [dcl.init.general]).
// STDOUT: fM
// EXIT: 0
extern "C" int putchar(int);

int calls;

int f() {
  putchar('f');
  ++calls;
  return 1;
}

int value = f() + 2;

int main() {
  putchar('M');
  putchar('\n');
  if (value != 3) return 1;
  if (calls != 1) return 2;
  value = 10;
  if (value != 10) return 3;
  return 0;
}
