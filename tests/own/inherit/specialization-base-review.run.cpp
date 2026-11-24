// Specialization bases meet the neighbouring rules: a primary template or a partial
// specialization whose base is another specialization of the same template, and unqualified
// lookup that skips a dependent base ([class.derived.general], [temp.dep]/3) (#1134).
// EXIT: 0
int f() { return 1; }

template<class T> struct Q;
template<> struct Q<int> { int u = 1; static int f() { return 2; } };
template<class T> struct Q : Q<int> {};             // the primary's base is a specialization of Q
template<class T> struct Q<T*> : Q<int> {};         // as is a partial specialization's

template<class T> struct Box { static int f() { return 3; } };
template<class T> struct R;
template<class T> struct R<T*> : Box<T> {
  static int g() { return f(); }                    // the dependent base is not searched
  static int h() { return R::f(); }                 // the qualified name finds it
};

int main() {
  Q<char> q;
  Q<long*> p;
  return q.u == 1 && p.u == 1 && R<int*>::g() == 1 && R<int*>::h() == 3 ? 0 : 1;
}
