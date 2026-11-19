// ζ a5 (#88), from the step's review: qualified names of a class's own member types in its
// member-specification, in its template's out-of-class member definitions, and through an alias
// template while the class is being defined or instantiated ([temp.dep.type]/1, [class.mem]).
// EXIT: 0
template<class U> struct C { using T = U; T g(U); };
template<class U> typename C<U>::T C<U>::g(U a) { return a * 2; }
template<class T> struct Node;
template<class T> using NodePtr = typename Node<T>::pointer;
template<class T> struct Node { using pointer = Node*; NodePtr<T> next; T v; };
template<class V> using VT = typename V::V2;
struct S { using V2 = int; VT<S> x; S::V2 y; };
template<class U> struct Q { using T = U; typename Q::T a; Q::T b; };
template<class T> struct A { typedef T* U; typename A<T>::U s; };
int main() {
  C<int> c;
  Node<int> b{nullptr, 2};
  Node<int> a{&b, 3};
  S s{1, 2};
  Q<int> q{3, 4};
  int i = 5;
  A<int> pa{&i};
  return c.g(2) + a.v + a.next->v + s.x + s.y + q.a + q.b + *pa.s == 4 + 5 + 3 + 7 + 5 ? 0 : 1;
}
