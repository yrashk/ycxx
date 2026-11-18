// ζ s3 (#88), from the step's review: a member of a class template specialization explicitly
// specialized, whether the template defines it in the class or outside it, and declared before
// it is defined ([temp.expl.spec]).
// EXIT: 0
template<class T> struct S { int m() const { return 4; } };
template<> int S<int>::m() const { return 6; }
template<class T> struct U { int m(); };
template<class T> int U<T>::m() { return 4; }
template<> int U<int>::m();
template<> int U<int>::m() { return 6; }
int main() {
  S<int> a;
  S<char> b;
  U<int> c;
  U<char> d;
  return a.m() * 10 + b.m() == 64 && c.m() * 10 + d.m() == 64 ? 0 : 1;
}
