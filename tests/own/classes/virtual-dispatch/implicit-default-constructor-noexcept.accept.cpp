// #309 [except.spec]/8: the implicit default constructor is potentially throwing exactly when the
// default initialization of a subobject invokes a potentially throwing function.
struct Throwing {
  Throwing() {}
};

struct Wrapper {
  Throwing t;
};

struct Polymorphic {
  virtual int f() { return 0; }
  Wrapper w;
};
static_assert(!noexcept(Polymorphic()));

struct Plain {
  int x;
};

struct Quiet {
  virtual int f() { return 0; }
  Plain p;
  int y;
};
static_assert(noexcept(Quiet()));
