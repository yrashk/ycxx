// ζ m9 (#88, #314): member function templates of classes and class templates ([temp.mem]):
// deduced through an object or the class, with explicit arguments after `.f<…>` or `.template
// f<…>`, static ones, overloads with ordinary members, and a class template's member template
// that uses both levels of arguments.
// EXIT: 0
struct S {
  int v = 1;
  template<class T> T get(T t) const { return t + T(v); }
  template<class T> static int st(T) { return 2; }
  int over(int) { return 10; }
  template<class T> int over(T) { return 20; }
};
template<class U> struct W {
  U u{};
  template<class T> int f(T t) { return int(sizeof(U)) * 100 + int(t); }
  template<class T> T conv() { return T(u); }
};
template<class T> int via_template(W<T>& w) { return w.template f<int>(3); }
int main() {
  S s;
  W<long> w;
  w.u = 7;
  if (s.get(1) != 2 || s.get<long>(3) != 4 || S::st(1L) != 2 || s.st('c') != 2) return 1;
  if (s.over(1) != 10 || s.over(1L) != 20 || s.over<int>(1) != 20) return 2;
  if (w.f(5) != 805 || w.conv<int>() != 7 || via_template(w) != 803) return 3;
  return 0;
}
