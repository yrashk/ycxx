// #309 [class.virtual], [class.mfct]: virtual member functions defined out of class (the first
// non-inline, non-pure one is the key function), mixed with inline ones, and overriders defined
// out of class.
// EXIT: 0

struct Base {
  virtual int inline_first() { return 1; }
  virtual int key();
  virtual int other();
  int plain();
};

int Base::key() { return 10; }
int Base::other() { return 100 + key(); }
int Base::plain() { return inline_first() + key(); }

struct Derived : Base {
  int key() override;
  int inline_first() override { return 2; }
};

int Derived::key() { return 20; }

struct MoreDerived : Derived {
  int other() override;
};

int MoreDerived::other() { return 300 + Base::other(); }

int call_all(Base& b) { return b.inline_first() + b.key() + b.other(); }

int main() {
  Base b;
  Derived d;
  MoreDerived m;
  if (call_all(b) != 1 + 10 + 110) return 1;
  if (call_all(d) != 2 + 20 + 120) return 2;
  if (call_all(m) != 2 + 20 + 420) return 3;
  if (b.plain() != 11) return 4;
  if (d.plain() != 22) return 5;
  Base* p = &m;
  if (p->plain() != 22) return 6;
  return 0;
}
