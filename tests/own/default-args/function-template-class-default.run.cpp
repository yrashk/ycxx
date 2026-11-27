// #356: a namespace-scope function template whose default argument depends on its template
// parameter and has class type: a class template specialization A<T>() and a class passed by
// value. Each use of the default constructs its object anew; a prvalue default initializes the
// parameter directly ([dcl.fct.default], [temp.inst], [dcl.init.general], [expr.call]).
// EXIT: 0
int made = 0, gone = 0;

template<class T>
struct A {
  int v;
  A() : v(static_cast<int>(sizeof(T))) { ++made; }
  A(const A& o) : v(o.v) { ++made; }
  ~A() { ++gone; }
};

template<class T>
int size_of(A<T> a = A<T>()) { return a.v; }

struct R {
  int v;
  R(int x) : v(x) { ++made; }
  R(const R& o) : v(o.v) { ++made; }
  ~R() { ++gone; }
};

template<class T>
int by_value(R r = R(sizeof(T) + 1)) { return r.v; }

int main() {
  if (size_of<char>() != 1) return 1;
  if (made != 1 || gone != 1) return 2;
  if (size_of<int>() != static_cast<int>(sizeof(int))) return 3;
  if (made != 2 || gone != 2) return 4;
  if (by_value<short>() != static_cast<int>(sizeof(short)) + 1) return 5;
  if (made != 3 || gone != 3) return 6;
  if (by_value<char>(R(40)) != 40) return 7;
  if (made != 4 || gone != 4) return 8;
  return 0;
}
