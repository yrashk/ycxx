// ι (#94), spec-only: static data members defined out of class with a dynamic initializer
// (`int C::m = f();`) are initialized before main, in the order of their definitions among the
// other namespace-scope variables of the translation unit, and a class-type one is destroyed in
// reverse order after main ([basic.start.dynamic], [basic.start.term], [class.static.data]).
// STDOUT: abcde
// STDOUT: M
// STDOUT: EB
// EXIT: 0
extern "C" int putchar(int);

int count;

int f(int c) {
  putchar(c);
  return ++count;
}

struct Box {
  int c;
  Box(int x) : c(x) { putchar(x); }
  ~Box() {
    putchar(c - 'a' + 'A');
    if (c == 'b') putchar('\n');
  }
};

struct C {
  static int m;
  static Box box;
  static long n;
  static const int k;
};

int before = f('a');
Box C::box('b');
int C::m = f('c');
long C::n = C::m * 100 + before;
const int C::k = f('d') + C::box.c;
Box after('e');

int main() {
  putchar('\n');
  putchar('M');
  putchar('\n');
  if (before != 1) return 1;
  if (C::m != 2) return 2;
  if (C::n != 201) return 3;
  if (C::k != 3 + 'b') return 4;
  if (C::box.c != 'b') return 5;
  C::m = 9;
  if (C::m != 9) return 6;
  return 0;
}
