// ι (#291): a static block array of class type is constructed element by element on the first
// pass ([dcl.init.aggr], [stmt.dcl]/3) and destroyed after main in reverse order of the
// elements' construction ([basic.start.term]/1, [class.dtor]); a static whose constructor takes
// the call's arguments is constructed from the first call's.
// EXIT: 0
// STDOUT: +1
// STDOUT: +2
// STDOUT: +3
// STDOUT: +7
// STDOUT: sum 6 7
// STDOUT: -7
// STDOUT: -3
// STDOUT: -2
// STDOUT: -1
extern "C" int putchar(int);

struct Noisy {
  int id;
  Noisy(int i) : id(i) {
    putchar('+');
    putchar('0' + id);
    putchar('\n');
  }
  ~Noisy() {
    putchar('-');
    putchar('0' + id);
    putchar('\n');
  }
};

int sum3() {
  static Noisy arr[3] = {1, 2, 3};
  return arr[0].id + arr[1].id + arr[2].id;
}

int keep(int first) {
  static Noisy k(first);
  return k.id;
}

int main() {
  int s = sum3();
  s = sum3();
  int k = keep(7);
  k = keep(8);
  putchar('s');
  putchar('u');
  putchar('m');
  putchar(' ');
  putchar('0' + s);
  putchar(' ');
  putchar('0' + k);
  putchar('\n');
  return 0;
}
