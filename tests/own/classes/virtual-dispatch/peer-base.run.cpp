// #309 [class.virtual], [class.cdtor], mixed-object ABI (DECISIONS §5.4): the peer (Clang) defines
// the base's key function, its other out-of-line virtuals and its constructor and destructor; this
// file derives from it, constructs a Derived, and passes it to the peer's `drive(Base&)`.
// PEER: peer-base-peer.cpp
// EXIT: 0

int trace;

struct Base {
  int scale;
  Base(int s);
  virtual ~Base();
  virtual int key();
  virtual int twice();
  virtual int label() { return 7; }
  int seen_in_ctor = 0;
};

int drive(Base& b);

struct Derived : Base {
  int bonus = 50;
  Derived() : Base(3) {}
  ~Derived() override { trace = trace * 10 + 2; }
  int key() override { return 100 + bonus; }
  int label() override { return 9; }
};

struct Leaf : Derived {
  int twice() override { return 1; }
};

int main() {
  {
    Derived d;
    // The peer's Base constructor records label() as seen during construction: Base's own, 7.
    if (d.seen_in_ctor != 7) return 1;
    // drive: key() * 100 + twice() * 10 + label(); Base::twice() returns 2 * key() % 10.
    if (drive(d) != 150 * 100 + 0 * 10 + 9) return 2;
    Base b(4);
    if (drive(b) != 4 * 100 + 8 * 10 + 7) return 3;
    trace = 0;
  }
  // b's destructor (peer, appends 1), then d's: Derived (2) then Base (1).
  if (trace != 121) return 4;
  trace = 0;
  {
    Leaf l;
    if (drive(l) != 150 * 100 + 1 * 10 + 9) return 5;
    trace = 0;
  }
  if (trace != 21) return 6;
  return 0;
}
