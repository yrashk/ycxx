// ι (#94), spec-only: a namespace-scope pointer bound by a non-constant initializer points to
// what the function chose; a temporary created in a dynamic initializer is destroyed at the end of
// that full-expression, before main ([basic.start.dynamic], [class.temporary]). The reference
// part is in references.run.cpp.
// STDOUT: tTuUM
// EXIT: 0
extern "C" int putchar(int);

int left = 1;
int right = 2;
int which;

int& pick() {
  ++which;
  return which == 1 ? right : left;
}

int* choose() { return &pick(); }

struct Temp {
  int v;
  Temp(int x) : v(x) { putchar(x == 5 ? 't' : 'u'); }
  ~Temp() { putchar(v == 5 ? 'T' : 'U'); }
  int get() const { return v * 2; }
};

int* ptr = choose();
int from_temp = Temp(5).get();
int from_temp2 = Temp(6).get() + from_temp;

int main() {
  putchar('M');
  putchar('\n');
  if (ptr != &right) return 2;  // the first call picks `right`
  if (*ptr != 2) return 4;
  if (from_temp != 10) return 5;
  if (from_temp2 != 22) return 6;
  return 0;
}
