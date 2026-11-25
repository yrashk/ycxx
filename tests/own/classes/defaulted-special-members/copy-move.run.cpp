// #157 [dcl.fct.def.default], [class.copy.ctor]/14, [class.copy.assign]/12: defaulted copy and move
// constructors and assignment operators use each subobject's own operation, in order.
// EXIT: 0
int log = 0;
void note(int step) { log = log * 10 + step; }
int take() {
  const int seen = log;
  log = 0;
  return seen;
}

struct Part {
  int value;
  Part(int v) : value(v) {}
  Part(const Part& o) : value(o.value) { note(1); }
  Part(Part&& o) : value(o.value) { o.value = 0; note(2); }
  Part& operator=(const Part& o) { value = o.value; note(3); return *this; }
  Part& operator=(Part&& o) { value = o.value; o.value = 0; note(4); return *this; }
};

struct Trivial {
  int a;
  long b[3];
  Trivial(int v) : a(v) { for (long& e : b) e = v; }
  Trivial(Trivial&&) = default;
  Trivial(const Trivial&) = default;
  Trivial& operator=(Trivial&&) = default;
  Trivial& operator=(const Trivial&) = default;
};

struct Base {
  Part b;
  Base(int v) : b(v) {}
  Base(const Base&) = default;
  Base(Base&&) = default;
  Base& operator=(const Base&) = default;
  Base& operator=(Base&&) = default;
};

struct Whole : Base {
  Part p;
  Part pair[2];
  char text[4];
  int& ref;
  Whole(int v, int& r) : Base(v), p(v + 1), pair{v + 2, v + 3}, text{'a', 'b', 'c', 0}, ref(r) {}
  Whole(const Whole&) = default;
  Whole(Whole&&) = default;
};

struct Assigned : Base {
  Part p;
  Part pair[2];
  short numbers[3];
  Assigned(int v) : Base(v), p(v + 1), pair{v + 2, v + 3}, numbers{1, 2, short(v)} {}
  Assigned& operator=(const Assigned&) = default;
  Assigned& operator=(Assigned&&) = default;
};

// A constructor template never takes the place of the copy of a base class subobject.
struct Greedy {
  int kind;
  Greedy() : kind(0) {}
  Greedy(const Greedy&) : kind(1) {}
  Greedy(Greedy&&) : kind(2) {}
  template <class T> Greedy(T&&) : kind(3) {}
};
struct FromGreedy : Greedy {
  FromGreedy() : Greedy() {}
  FromGreedy(const FromGreedy&) = default;
  FromGreedy(FromGreedy&&) = default;
};

// Only a move: the copy is deleted. A member without a move is copied.
struct OnlyCopy {
  int v;
  OnlyCopy(int x) : v(x) {}
  OnlyCopy(const OnlyCopy& o) : v(o.v + 100) {}
};
struct Mover {
  OnlyCopy c;
  Mover(int v) : c(v) {}
  Mover(Mover&&) = default;
};

// A defaulted move that is deleted is ignored: an rvalue is copied.
struct NoMove {
  int v;
  NoMove(int x) : v(x) {}
  NoMove(const NoMove& o) : v(o.v + 10) {}
  NoMove(NoMove&&) = delete;
  NoMove& operator=(const NoMove& o) { v = o.v + 20; return *this; }
private:
  NoMove& operator=(NoMove&&);
};
struct Copied {
  NoMove n;
  Copied(int v) : n(v) {}
  Copied(const Copied&) = default;
  Copied(Copied&&) = default;
  Copied& operator=(const Copied&) = default;
  Copied& operator=(Copied&&) = default;
};

// A non-const parameter, and one for const beside it.
struct Both {
  int how;
  Both() : how(0) {}
  Both(Both&) = default;
  Both(const Both& o) : how(o.how + 1) {}
};

struct Qualified {
  int v;
  Qualified& operator=(const Qualified&) & = default;
  Qualified& operator=(Qualified&&) & = default;
};

int main() {
  Trivial t(3);
  Trivial u(t);
  Trivial v(static_cast<Trivial&&>(t));
  u = v;
  u.b[2] = 9;
  v = static_cast<Trivial&&>(u);
  if (v.a != 3 || v.b[0] != 3 || v.b[2] != 9) return 1;

  int target = 7;
  Whole w(4, target);
  Whole x(w);
  if (take() != 1111) return 2;
  if (x.b.value != 4 || x.p.value != 5 || x.pair[0].value != 6 || x.pair[1].value != 7) return 3;
  if (x.text[2] != 'c' || &x.ref != &target) return 4;
  Whole y(static_cast<Whole&&>(x));
  if (take() != 2222) return 5;
  if (x.b.value != 0 || x.pair[1].value != 0 || y.b.value != 4 || y.pair[1].value != 7 || &y.ref != &target) return 6;

  Assigned a(10), b(20);
  Assigned& r = (a = b);
  if (&r != &a || take() != 3333) return 7;
  if (a.b.value != 20 || a.pair[1].value != 23 || a.numbers[2] != 20 || a.numbers[1] != 2) return 8;
  a = static_cast<Assigned&&>(b);
  if (take() != 4444 || b.b.value != 0 || b.pair[0].value != 0 || a.pair[0].value != 22) return 9;

  FromGreedy g;
  FromGreedy h(g);
  FromGreedy i(static_cast<FromGreedy&&>(g));
  if (h.kind != 1 || i.kind != 2) return 10;

  Mover m(5);
  Mover n(static_cast<Mover&&>(m));
  if (n.c.v != 105 || m.c.v != 5) return 11;

  Copied c(1);
  Copied d(static_cast<Copied&&>(c));
  if (d.n.v != 11) return 12;
  d = static_cast<Copied&&>(c);
  if (d.n.v != 21) return 13;

  Both e;
  const Both f;
  Both e2(e), f2(f);
  if (e2.how != 0 || f2.how != 1) return 14;

  Qualified q{6}, s{0};
  s = q;
  if (s.v != 6) return 15;
  q.v = 8;
  s = static_cast<Qualified&&>(q);
  return s.v == 8 ? 0 : 16;
}
