// #1153 [except.spec]: a specialization's virtual member is nonthrowing, its overrider is not.
template <class T> struct A {
  virtual int f() noexcept(sizeof(T) > 1) { return 1; }
};
struct Fine : A<char> {
  int f() override { return 2; }
};
struct Looser : A<int> {
  int f() override { return 3; } //! error
};
