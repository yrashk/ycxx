// #1417 review [temp.over.link]/6, [class.mfct]: out-of-class constructors of a class template
// whose head has its own requires-clause, one of them with a trailing requires-clause.
template<class T> concept Any = true;
template<class T> concept Small = sizeof(T) <= 2;
template<class T> requires Any<T> struct S {
  S(int) requires Small<T>;
  S(long);
  int v = 0;
};
template<class T> requires Any<T> S<T>::S(int) requires Small<T> : v(1) {}
template<class T> requires Any<T> S<T>::S(long) : v(2) {}
int main() {
  S<char> a(1);
  S<int> b(1L);
  return a.v == 1 && b.v == 2 ? 0 : 1;
}
