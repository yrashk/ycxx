// #1417 [dcl.decl.general], [temp.mem], [over.match.best.general], [temp.func.order]: a member
// function template of a class template with a trailing requires-clause that names both the
// enclosing class template's parameter and the member template's own parameter.
template<class A, class B> concept SameSize = sizeof(A) == sizeof(B);

template<class T>
struct Pair {
  template<class U> static int match(U) requires SameSize<T, U> { return 1; }
  template<class U> static int match(U) requires (!SameSize<T, U>) { return 2; }
};

int main() {
  if (Pair<int>::match(1u) != 1) return 1;
  if (Pair<int>::match('c') != 2) return 2;
  if (Pair<char>::match('c') != 1) return 3;
  return 0;
}
