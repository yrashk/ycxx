// ι (#94), spec-only: namespace-scope class objects with user-provided constructors, in the forms
// `S s;`, `S s(1, 2);`, `S s = S(3);` and `S s{4};`, are constructed before main in the order of
// their definitions ([basic.start.dynamic], [dcl.init.general], [class.ctor]); `S(3)` initializes
// the object directly, with no copy and no temporary ([dcl.init.general], [class.temporary]).
// STDOUT: dpcbM
// STDOUT: ~~~~
// EXIT: 0
extern "C" int putchar(int);

int made;

struct S {
  int a;
  int b;
  int order;
  S() : a(0), b(0), order(++made) { putchar('d'); }
  S(int x, int y) : a(x), b(y), order(++made) { putchar('p'); }
  S(int x) : a(x), b(-x), order(++made) { putchar(x == 3 ? 'c' : 'b'); }
  S(const S& other) : a(other.a), b(other.b), order(++made) { putchar('!'); }
  ~S() {
    putchar('~');
    if (order == 1) putchar('\n');
  }
};

S plain;
S paren(1, 2);
S copied = S(3);
S braced{4};

int main() {
  putchar('M');
  putchar('\n');
  if (plain.a != 0 || plain.b != 0 || plain.order != 1) return 1;
  if (paren.a != 1 || paren.b != 2 || paren.order != 2) return 2;
  if (copied.a != 3 || copied.b != -3 || copied.order != 3) return 3;
  if (braced.a != 4 || braced.b != -4 || braced.order != 4) return 4;
  if (made != 4) return 5;
  return 0;
}
