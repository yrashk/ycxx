// [class.access.general]: access applies to the named alias, including an unevaluated operand.
using T = int;
struct Base {
private:
  using T = char;
};
struct Derived : Base {
  int member() { return sizeof(T); } //! error
};
