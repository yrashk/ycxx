// #340 item 3: ref-qualified conversion function templates are chosen by the object's value
// category through the implicit object parameter ([over.match.funcs.general]/5,
// [over.ics.rank]/3.2.3), as libycxx's ranges result types declare them ([algorithms.results]).
// EXIT: 0
template <class T> struct R {
  T t;
  template <class U> constexpr operator R<U>() const& { return {U(t + 10)}; }
  template <class U> constexpr operator R<U>() && { return {U(t + 20)}; }
};
template <class T> T&& move(T& t) { return static_cast<T&&>(t); }
int main() {
  R<int> a{1};
  R<long> b = a;
  R<long> c = R<int>{1};
  R<long> d = move(a);
  const R<int> e{2};
  R<char> f = e;
  return b.t == 11 && c.t == 21 && d.t == 21 && f.t == 12 ? 0 : 1;
}
