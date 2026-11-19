// ζ m9 (#88, #314), from the step's review: explicit template arguments after S::f, N::f, an unqualified member template,
// and S::template f / X::template f ([temp.arg.explicit], [temp.names]/3).
// EXIT: 0
namespace N { template<class T> int nf(T) { return 1; } }
struct S {
  template<class T> static int st(T) { return int(sizeof(T)); }
  template<class T> int f(T) { return 2; }
  int g() { return f<int>(1) + st<char>(1); }
};
template<class V> struct W { template<class T> static int mk(T) { return int(sizeof(T)); } };
template<class X> int dep() { return X::template st<long>(1); }
int main() { S s; return S::st<long>(1) + W<int>::mk<short>(1) + s.g() + N::nf<long>(1) + S::template st<char>(1) + dep<S>() - 23; }
