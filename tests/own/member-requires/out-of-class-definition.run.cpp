// #1417 [temp.mem.func], [temp.over.link], [basic.scope.scope], [dcl.decl.general]: members
// declared in a class template with trailing requires-clauses and defined outside it; each
// definition repeats the requires-clause and matches the declaration with the equivalent
// clause, so overloads differing only in constraints get distinct definitions.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct S {
  int f() requires Small<T>;
  int f() requires (!Small<T>);
  static int g() requires Small<T>;
  S(int) requires Small<T>;
  S(int) requires (!Small<T>);
  int v;
};

template<class T> int S<T>::f() requires Small<T> { return 1; }
template<class T> int S<T>::f() requires (!Small<T>) { return 2; }
template<class T> int S<T>::g() requires Small<T> { return 3; }
template<class T> S<T>::S(int x) requires Small<T> : v(x) {}
template<class T> S<T>::S(int x) requires (!Small<T>) : v(x * 2) {}

int main() {
  S<char> a(5);
  S<int> b(6);
  if (a.f() != 1 || a.v != 5) return 1;
  if (b.f() != 2 || b.v != 12) return 2;
  if (S<short>::g() != 3) return 3;
  return 0;
}
