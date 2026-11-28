// #340 item 3: a conversion function template of a class template ([class.conv.fct], [temp.mem]/5)
// copy-initializes another specialization ([over.match.copy]/1.2). Its template argument is
// deduced from the type the conversion requires ([temp.deduct.conv]).
// EXIT: 0
template <class T> struct R {
  T t;
  template <class U> constexpr operator R<U>() const& { return {U(t)}; }
};
template <class T> struct Box {
  T v;
};
struct Maker {
  int n;
  template <class T> operator Box<T>() const { return {T(n + 1)}; }
};
int main() {
  R<int> a{1};
  R<long> b = a;
  const R<int> c{2};
  R<short> d = c;
  Maker m{4};
  Box<long> e = m;
  Box<char> f = Maker{6};
  return b.t + d.t + e.v + f.v == 1 + 2 + 5 + 7 ? 0 : 1;
}
