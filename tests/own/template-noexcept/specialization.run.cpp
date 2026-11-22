// #828: a function template specialization, and a member of a class template specialization,
// has the exception specification its declaration gives, with a dependent condition evaluated
// with the specialization's arguments when it is needed ([except.spec]/13, [temp.inst]). The
// unary `noexcept` operator reads it ([expr.unary.noexcept]). Each check agrees with Clang and GCC.
// EXIT: 0
template<class T> void bare(T) noexcept {}
template<class T> void plain(T) {}
template<bool B> void value() noexcept(B) {}
template<class T> void sized() noexcept(sizeof(T) == 1) {}
template<class T> void param(T t) noexcept(noexcept(t + 1)) {}
template<class T> struct W {
  void f() noexcept(sizeof(T) > 1) {}
  static void g() noexcept(sizeof(T) == 1) {}
  void h() noexcept {}
  void k() {}
};
// A non-dependent noexcept on a pointer's function type in a template (#819).
int nothrow_fn() noexcept { return 0; }
template<class T> int pointer() {
  int (*p)() noexcept = nothrow_fn;
  return noexcept(p()) ? 1 : 0;
}
// A condition naming another specialization's.
template<class T> void outer() noexcept(noexcept(sized<T>())) {}

int main() {
  if (!noexcept(bare(1)) || noexcept(plain(1))) return 1;
  if (!noexcept(value<true>()) || noexcept(value<false>())) return 2;
  if (!noexcept(sized<char>()) || noexcept(sized<int>())) return 3;
  if (!noexcept(param(1))) return 4;
  W<int> wi;
  W<char> wc;
  if (!noexcept(wi.f()) || noexcept(wc.f())) return 5;
  if (!noexcept(W<char>::g()) || noexcept(W<int>::g())) return 6;
  if (!noexcept(wi.h()) || noexcept(wi.k())) return 7;
  if (!noexcept(outer<char>()) || noexcept(outer<int>())) return 8;
  if (pointer<int>() != 1) return 9;
  // The pointer to a non-throwing specialization is a pointer to noexcept function.
  void (*p)() noexcept = sized<char>;
  void (*q)() noexcept = value<true>;
  p();
  q();
  return 0;
}
