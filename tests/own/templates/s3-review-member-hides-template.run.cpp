// ζ s3 (#88), from the step's review: a class member hides a namespace's variable template of
// its name, so `<` after it is a comparison ([basic.scope.class], [temp.names]).
// EXIT: 0
template<class T> constexpr int v = 1;
struct A {
  int v = 5;
  int f() { return v < 2; }
  int g();
};
int A::g() { return v < 9; }
int main() {
  A a;
  return a.f() * 10 + a.g() == 1 ? 0 : 1;
}
