// ζ s3 (#88): members of a partial specialization defined outside it, members of the primary
// template defined outside it, and a member of a specialization explicitly specialized; each
// specialization uses the definitions of the pattern it is instantiated from
// ([temp.spec.partial.member], [temp.expl.spec]).
// EXIT: 0
template<class T, int I>
struct A {
  int f();
};
template<class T, int I>
int A<T, I>::f() { return I; }

template<class T>
struct A<T, 2> {
  int g();
  int h();
};
template<class T>
int A<T, 2>::g() { return 20; }
template<class T>
int A<T, 2>::h() { return 30; }
template<>
int A<char, 2>::h() { return 31; }

int main() {
  A<char, 0> a0;
  A<char, 2> a2;
  A<int, 2> b2;
  if (a0.f() != 0 || a2.g() != 20 || a2.h() != 31) return 1;
  return b2.h() == 30 ? 0 : 2;
}
