// #297: partial ordering compares a pack-index-specifier through the unique type standing in for
// its pack ([temp.func.order]/3): the first `f` is more specialized than the second.
// EXIT: 0
template<class... T> int f(T...[0]*, T...) { return 1; }
template<class... T> int f(T...) { return 2; }
template<class A, class... T> int g(A*, T...[0]) { return 3; }
template<class A, class B> int g(A, B) { return 4; }
int main() {
  int x = 0;
  if (f(&x, 1) != 1 || f(1, 2) != 2) return 1;
  if (g<int, char>(&x, 'c') != 3) return 2;
  return 0;
}
