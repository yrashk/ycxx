// #340 item 3: copy-initialization through conversion function templates fails when no candidate
// is viable: a constraint that is not satisfied removes it ([over.match.viable]/3), the object's
// value category must fit its ref-qualifier ([over.match.funcs.general]/5); and when two remain
// that neither beats ([over.match.best.general]/2). (An explicit conversion function template
// does not parse yet: #1723.)
template <class T> struct R {
  T t;
};
struct C {
  template <class T> requires (sizeof(T) == 1) operator T() const { return T(); }
};
struct L {
  template <class T> operator R<T>() & { return {}; }
};
struct A {
  template <class T> operator T() const { return T(); }
  template <class T> operator const T&() const { static T t; return t; }
};
int main() {
  C c;
  char ok = c;
  int bad = c; //! error
  R<int> b = L{}; //! error
  A x;
  int amb = x; //! error
  return ok;
}
