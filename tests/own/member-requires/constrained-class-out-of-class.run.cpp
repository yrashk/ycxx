// #1417 review [temp.over.link]/6, [class.mfct]: an out-of-class definition of a constrained
// member of a class template whose head has its own requires-clause matches by the member's
// trailing requires-clause alone.
template<class T> concept Any = true;
template<class T> concept Small = sizeof(T) <= 2;
template<class T> requires Any<T> struct S {
  int f() requires Small<T>;
  int f() requires (!Small<T>);
};
template<class T> requires Any<T> int S<T>::f() requires Small<T> { return 1; }
template<class T> requires Any<T> int S<T>::f() requires (!Small<T>) { return 2; }
int main() { return S<char>{}.f() == 1 && S<int>{}.f() == 2 ? 0 : 1; }
