// The other side of decay-copy-constraints.run.cpp, compiled by Clang: declarations only, so
// every call below names a specialization defined by the test's object.
template<class T> T& lvalue() noexcept;
struct Range { int* begin(); int* data(); };

template<class T> requires requires { auto(lvalue<T>().begin()); } int parens(T&);
template<class T> requires requires { { auto{lvalue<T>().data()} }; } int braces(T&);

int peer() {
  Range r;
  return parens(r) + braces(r) == 3 ? 0 : 11;
}
