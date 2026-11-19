// ι (#94), spec-only: ordered dynamic initialization: within one translation unit, namespace-scope
// variables are initialized in the order of their definitions ([basic.start.dynamic]); a later
// initializer reads the value an earlier one stored.
// STDOUT: abcdM
// EXIT: 0
extern "C" int putchar(int);

int step;

int mark(int c) {
  putchar(c);
  return ++step;
}

int first = mark('a');
int second = mark('b');
int sum = first + second + mark('c');
long wide = sum * 10 + mark('d');

int main() {
  putchar('M');
  putchar('\n');
  if (first != 1) return 1;
  if (second != 2) return 2;
  if (sum != 6) return 3;
  if (wide != 64) return 4;
  if (step != 4) return 5;
  return 0;
}
