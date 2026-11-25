// YCXX-only coverage gate: what this slice does not lower is diagnosed, never miscompiled.
// Deletion through a virtual destructor needs the deleting destructor to select the dynamic
// type's deallocation function ([expr.delete]/10; #931, #309). A braced argument in a
// parenthesized new-initializer needs list-initialization of the parameter (#293).
// XFAIL: clang Clang supports these.
// XFAIL: gcc GCC supports these.
struct Base {
  virtual ~Base();
};
void release(Base* b) { delete b; } //! error

struct Il {
  int a, b;
};
Il* braced() { return new Il({1, 2}); } //! error
