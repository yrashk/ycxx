// ζ f1 (#88), from the step's review: a call of a template-id with a dependent argument is a
// dependent call, so its instantiation adds argument-dependent lookup ([temp.dep.general]/2,
// [temp.dep.candidate]): A::h is a better match than ::h.
// EXIT: 0
template<class T> int h(const void*) { return 1; }
template<class T> int f(T* t) { return h<T>(t); }
namespace A {
struct B {};
template<class T> int h(B*) { return 2; }
}
int main() {
  A::B b;
  if (f(&b) != 2) return 1;
  int i = 0;
  if (f(&i) != 1) return 2;
  return 0;
}
