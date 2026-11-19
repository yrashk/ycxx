// ι (#94), spec-only: namespace-scope aggregates and arrays with non-constant initializers: the
// initializer-clauses of a braced list are evaluated in order ([dcl.init.list]), elements and
// members not explicitly initialized are zero ([dcl.init.aggr]); an array of class objects is
// constructed element by element in order and destroyed in reverse after main
// ([basic.start.dynamic], [basic.start.term], [class.init]).
// STDOUT: 12345abc
// STDOUT: M
// STDOUT: CBA
// EXIT: 0
extern "C" int putchar(int);

int count;

int next() {
  ++count;
  putchar('0' + count);
  return count;
}

struct Pair {
  int x;
  int y;
  int z;
};

int made;

struct Elem {
  char c;
  Elem() : c('a' + made++) { putchar(c); }
  ~Elem() {
    putchar(c - 'a' + 'A');
    if (c == 'a') putchar('\n');
  }
};

int nums[4] = {next(), next(), next()};
Pair pair = {next(), next()};
Elem elems[3];

int main() {
  putchar('\n');
  putchar('M');
  putchar('\n');
  if (nums[0] != 1 || nums[1] != 2 || nums[2] != 3 || nums[3] != 0) return 1;
  if (pair.x != 4 || pair.y != 5 || pair.z != 0) return 2;
  if (elems[0].c != 'a' || elems[2].c != 'c') return 3;
  return 0;
}
