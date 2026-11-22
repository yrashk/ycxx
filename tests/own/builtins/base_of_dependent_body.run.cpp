// #218: `__is_base_of` with dependent operands in a function template body is evaluated when the
// body is instantiated ([meta.rel], [temp.inst]): in an expression, an `if constexpr` condition
// (as libycxx's `invoke.hpp` has it); with a private, an indirect and an
// unrelated class; with a class completed after the template's definition; in members of a
// class template, a member template and a generic lambda.
// EXIT: 0
struct B {};
struct M : B {};
struct D : private M {};
struct U {};
struct Late;

template<class X, class Y> constexpr int classify() {
  if constexpr (__is_same(X, Y) || __is_base_of(X, Y)) return 1;
  else return 2;
}
template<class X, class Y> bool related(X*, Y*) { return __is_base_of(X, Y) || __is_base_of(Y, X); }
template<class T> int late() { return __is_base_of(B, T) ? 3 : 4; }
template<class T> struct Holder {
  static int member() { return __is_base_of(B, T) ? 5 : 6; }
  template<class U> static int both() { return __is_base_of(T, U) + __is_base_of(U, T); }
};
template<class T> int lambda() {
  auto f = [](auto* p) { return __is_base_of(T, __remove_pointer(decltype(p))) ? 7 : 8; };
  return f(static_cast<D*>(nullptr));
}

struct Late : B {};

int main() {
  static_assert(classify<B, D>() == 1 && classify<D, B>() == 2 && classify<B, B>() == 1);
  static_assert(classify<int, int>() == 1 && classify<B, U>() == 2);
  B* b = nullptr;
  U* u = nullptr;
  D* d = nullptr;
  if (!related(b, d) || related(b, u)) return 1;
  if (late<Late>() != 3 || late<U>() != 4) return 2;
  if (Holder<M>::member() != 5 || Holder<U>::member() != 6 || Holder<B>::both<M>() != 1) return 3;
  if (lambda<B>() != 7 || lambda<U>() != 8) return 4;
  return 0;
}
