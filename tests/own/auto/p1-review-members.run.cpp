// θ step 1 (#279), from the review: in a class template, a member with a placeholder return type
// named by another member is type-dependent in the definition ([temp.dep.expr]/3); each
// specialization deduces it.
template<class T> struct S {
  T v;
  auto get() const { return v; }
  int twice() const { return get() * 2; }
  static auto a() { return 1; }
  static auto b() { return a() + 1; }
};
int main() { return S<int>{3}.twice() + S<long>::b() == 8 ? 0 : 1; }
