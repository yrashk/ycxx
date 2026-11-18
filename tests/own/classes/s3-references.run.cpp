// δ s3 (#86): lvalue references: aliases of locals and members, reference parameters that
// modify the argument, `const S&` parameters that copy nothing, functions returning a
// reference that can be assigned through, and overloading on `T&` versus `const T&`
// ([dcl.ref], [dcl.init.ref], [expr.call], [over.ics.rank]).
// EXIT: 0
int copies = 0;

struct Big {
  int data[4];
  Big() : data{1, 2, 3, 4} {}
  Big(const Big& o) : data{o.data[0], o.data[1], o.data[2], o.data[3]} { ++copies; }
  int& at(int i) { return data[i]; }
  int sum() const { return data[0] + data[1] + data[2] + data[3]; }
};

void swap_ints(int& a, int& b) {
  int t = a;
  a = b;
  b = t;
}

int total(const Big& b) { return b.sum(); }

int& larger(int& a, int& b) { return a > b ? a : b; }

int which(int&) { return 1; }
int which(const int&) { return 2; }

int main() {
  int x = 1, y = 2;
  int& r = x;
  r = 10;
  if (x != 10) return 1;
  if (&r != &x) return 2;
  swap_ints(x, y);
  if (x != 2 || y != 10) return 3;

  Big b;
  if (total(b) != 10) return 4;
  if (copies != 0) return 5;  // bound directly, no copy
  b.at(2) = 30;
  if (b.data[2] != 30 || total(b) != 37) return 6;
  Big& br = b;
  br.at(0) = 0;
  if (b.data[0] != 0) return 7;
  Big c = br;
  if (copies != 1 || c.sum() != 36) return 8;

  larger(x, y) = 0;
  if (y != 0 || x != 2) return 9;
  int& m = b.data[3];
  m += 1;
  if (b.at(3) != 5) return 10;

  const int k = 5;
  if (which(x) != 1) return 11;
  if (which(k) != 2) return 12;
  if (which(7) != 2) return 13;  // a temporary binds only to const int&
  const int& tmp = x + 100;
  if (tmp != 102) return 14;
  return 0;
}
