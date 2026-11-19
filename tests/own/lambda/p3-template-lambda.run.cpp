// θ step 3 (#281), spec-only: a lambda with a template-parameter-list has a function call operator
// template with those template parameters; T is deduced from each call's argument
// ([expr.prim.lambda.general], [expr.prim.lambda.closure], [temp.deduct.call]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto id = []<class T>(T x) { return x; };
  if (id(5) != 5) return 1;
  if (id('q') != 'q') return 2;
  if (!same<decltype(id('q')), char>::value) return 3;
  if (!same<decltype(id(5L)), long>::value) return 4;
  auto ptr = []<typename T>(T* p) { return *p; };
  int i = 11;
  char c = 'x';
  if (ptr(&i) != 11 || ptr(&c) != 'x') return 5;
  if (!same<decltype(ptr(&c)), char>::value) return 6;
  auto size = []<class T, int N>(T (&a)[N]) { return N * 100 + static_cast<int>(sizeof(T)); };
  int a3[3] = {};
  char a5[5] = {};
  if (size(a3) != 300 + static_cast<int>(sizeof(int))) return 7;
  if (size(a5) != 501) return 8;
  return 0;
}
