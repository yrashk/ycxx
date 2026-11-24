// Peer of peer-derived.run.cpp, compiled by Clang.

struct Base {
  int seed = 3;
  virtual int key();
  virtual int scaled(int k) { return k * seed; }
  virtual ~Base() {}
};

struct Derived : Base {
  int extra;
  Derived(int e);
  int key() override;
  int scaled(int k) override;
  ~Derived() override;
};

extern int trace;
int use_base(Base& b);

Derived::Derived(int e) : extra(e) {}
int Derived::key() { return 2 + extra; }
int Derived::scaled(int k) { return k + extra + Base::scaled(k); }
Derived::~Derived() { trace = trace * 10 + 5; }

int run_peer_derived(int e) {
  Derived d(e);
  return use_base(d);
}
