// YCXX-only coverage gate: deletion through a virtual destructor is diagnosed, never
// miscompiled. It needs the deleting destructor to select the dynamic type's deallocation
// function ([expr.delete]/10; #931, #309).
// XFAIL: clang Clang supports polymorphic deletion.
// XFAIL: gcc GCC supports polymorphic deletion.
struct Base {
  virtual ~Base();
};
void release(Base* b) { delete b; } //! error
