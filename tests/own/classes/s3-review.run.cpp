// δ s3 (#86), from the step's review: the implicit copy constructor copies array members element
// by element ([class.copy.ctor]/14); a temporary bound to a braced reference, or through one of
// its members, lives as long as the reference ([class.temporary]/6); an rvalue reference to an
// rvalue is the better binding ([over.ics.rank]/3.2.3); an object pointer converts to `void*`
// ([conv.ptr]).
// EXIT: 0
int log_[16];
int n = 0;
struct E {
  int v;
  E(int x) : v(x) {}
  E(const E& o) : v(o.v + 1) { log_[n++] = 7; }
};
struct H { E arr[2]; };
struct A {
  int x;
  A(int v) : x(v) { log_[n++] = v; }
  ~A() { log_[n++] = -x; }
};
int pick(int&&) { return 1; }
int pick(const int&) { return 2; }
struct S {
  int seen;
  int take(void* p) { return p != nullptr ? 1 : 0; }
};

int main() {
  H h{{E(1), E(2)}};
  H h2 = h;
  if (n != 2 || h2.arr[0].v != 2 || h2.arr[1].v != 3) return 1;
  n = 0;
  {
    const A& r{A(1)};
    if (n != 1 || r.x != 1) return 2;
    const int& m = A(2).x;
    if (n != 2 || m != 2 || r.x + m != 3) return 3;
  }
  if (n != 4 || log_[2] != -2 || log_[3] != -1) return 4;
  int i = 0;
  if (pick(1) != 1 || pick(i) != 2) return 5;
  S s{0};
  if (s.take(&i) != 1) return 6;
  return 0;
}
