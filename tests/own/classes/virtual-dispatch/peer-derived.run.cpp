// #309 [class.virtual], mixed-object ABI (DECISIONS §5.4): this file defines a polymorphic base
// with an out-of-line key function; the peer (Clang) defines a derived class overriding it and
// constructs one, calling back into `use_base` here, which makes the virtual calls.
// PEER: peer-derived-peer.cpp
// EXIT: 0

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

int trace;

int Base::key() { return 1; }

int use_base(Base& b) { return b.key() * 1000 + b.scaled(10); }

int run_peer_derived(int e);

int main() {
  Base b;
  if (use_base(b) != 1030) return 1;
  // The peer's Derived: key() == 2 + extra, scaled(k) == k + extra + Base::scaled(k);
  // its destructor appends 5 to trace, and use_base's result is returned.
  int r = run_peer_derived(4);
  if (r != 6 * 1000 + (10 + 4 + 30)) return 2;
  if (trace != 5) return 3;
  return 0;
}
