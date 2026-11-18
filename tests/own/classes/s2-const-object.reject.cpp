// δ s2 (#86): a non-const member function called on a const object or through a pointer to
// const, and assignment to a member inside a const member function, or of a const object
// ([class.this], [class.mfct.non.static], [expr.assign]).
struct Counter {
  int n;
  void inc() { ++n; }
  int get() const { return n; }
  void reset() const {
    n = 0; //! error
  }
};

int main() {
  const Counter c = {1};
  c.inc(); //! error
  const Counter* p = &c;
  p->inc(); //! error
  c.n = 2; //! error
  return c.get();
}
