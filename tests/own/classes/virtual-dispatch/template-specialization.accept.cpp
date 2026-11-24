// [class.virtual]: a class template specialization deriving from a polymorphic class.
struct Base {
  virtual ~Base() {}
  virtual int f() { return 1; }
};

template <class T> struct Holder : Base {
  T value{};
  int f() override { return 2; }
};

int use() {
  Holder<int> h;
  Base& b = h;
  return b.f();
}
