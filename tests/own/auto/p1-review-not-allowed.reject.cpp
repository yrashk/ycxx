// θ step 1 (#279), from the review: placeholders where [dcl.spec.auto.general] does not allow
// them: a non-static data member, a template argument, a cast, a parameter of a function type, a
// member typedef, and a function among several declarators.
template<class T> struct X {};
struct S {
  int a;
  auto b = 5; //! error
  typedef auto T; //! error
};
X<auto> x; //! error
void g(void (*p)(auto)); //! error
auto f(), h(); //! error
int main() {
  return (auto*)0 != 0; //! error
}
