// δ s3 (#86): implicitly declared default constructor, copy constructor, copy assignment and
// destructor: the default constructor runs members' constructors and default member
// initializers, copying copies memberwise using members' own copy constructors, the implicit
// destructor destroys members ([class.default.ctor], [class.copy.ctor], [class.copy.assign],
// [class.dtor]).
// EXIT: 0
int copies = 0;
int dtors = 0;

struct Copier {
  int v;
  Copier() : v(1) {}
  Copier(const Copier& o) : v(o.v) { ++copies; }
};

struct Bye {
  ~Bye() { ++dtors; }
};

struct Inner {
  int v;
  Inner() : v(2) {}
};

struct Holder {  // every special member function is implicit
  int count = 4;
  Copier c;
  int raw[2];
  Bye bye;
};

struct Plain {  // every special member function is implicit
  int count = 3;
  Inner in;
  int raw[2];
};

int main() {
  {
    Holder a;
    if (a.count != 4 || a.c.v != 1) return 1;
    a.c.v = 5;
    a.raw[0] = 6;
    a.raw[1] = 7;
    Holder b = a;  // implicit copy constructor
    if (copies != 1) return 2;
    if (b.c.v != 5 || b.raw[1] != 7 || b.count != 4) return 3;
    b.raw[0] = 0;
    if (a.raw[0] != 6) return 4;
  }
  if (dtors != 2) return 5;  // a.bye and b.bye, via Holder's implicit destructor

  Plain p;
  if (p.count != 3 || p.in.v != 2) return 6;
  p.in.v = 8;
  p.raw[0] = 9;
  p.raw[1] = 10;
  Plain q;
  q.count = 0;
  q = p;  // implicit copy assignment
  if (q.count != 3 || q.in.v != 8 || q.raw[0] != 9 || q.raw[1] != 10) return 7;
  q.raw[1] = 0;
  if (p.raw[1] != 10) return 8;
  return 0;
}
