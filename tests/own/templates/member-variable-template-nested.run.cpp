// #340: a static data member template of a class nested in a class template, depending on the
// enclosing template's parameter ([temp.mem], [temp.mem.class], [class.static.data]).
// EXIT: 0
template <class A> struct Outer {
  struct Inner {
    template <class B> static constexpr int v = int(sizeof(A)) * 10 + int(sizeof(B));
    static int body() { return v<char>; }
  };
  static int via_inner() { return Inner::template v<short>; }
};
static_assert(Outer<int>::Inner::v<char> == 41);
static_assert(Outer<char>::Inner::template v<int> == 14);
int main() {
  int r = Outer<short>::Inner::body() + Outer<int>::via_inner();
  // 21 + 42
  return r == 63 ? 0 : 1;
}
