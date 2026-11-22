// #807: a lambda's noexcept-specifier applies to its function call operator
// ([expr.prim.lambda.closure]/4); one that depends on an enclosing template's parameters is
// evaluated with each specialization's arguments ([except.spec], [temp.inst]). The unary
// `noexcept` operator reads it ([expr.unary.noexcept]), and a captureless lambda converts to a
// pointer to noexcept function when it is non-throwing ([expr.prim.lambda.closure]/8).
// Each check agrees with Clang and GCC.
// EXIT: 0
struct Base {};
struct Derived : Base {};

// The issue's example.
template<class T> int sized() {
  auto l = []() noexcept(sizeof(T) == 1) { return 0; };
  return noexcept(l()) ? 1 : 0;
}
// With the built-in traits #218's review found.
template<class T, class M> int same() {
  auto l = []() noexcept(__is_same(T, M)) { return 0; };
  return noexcept(l()) ? 1 : 0;
}
template<class T> int base_of() {
  auto l = []() noexcept(__is_base_of(Base, T)) { return 0; };
  return noexcept(l()) ? 1 : 0;
}
// A generic lambda whose condition names the enclosing template's parameter only.
template<class T> int generic() {
  auto l = [](auto x) noexcept(sizeof(T) == 1) { return x; };
  return noexcept(l(0)) ? 1 : 0;
}
// In a member function of a class template.
template<class T> struct Holder {
  static int get() {
    auto l = []() noexcept(sizeof(T) == 1) { return 0; };
    return noexcept(l()) ? 1 : 0;
  }
};
// A dependent check in the template's definition, decided by each instantiation.
template<class T> int asserted() {
  auto l = []() noexcept(sizeof(T) == 1) { return 0; };
  static_assert(noexcept(l()) == (sizeof(T) == 1));
  return 0;
}
// The conversion to a pointer to function, from the operator's noexcept function type.
template<class T> int converted() {
  auto l = []() noexcept(sizeof(T) == 1) { return 3; };
  int (*q)() = l;
  return q() + +l();
}

// Overloads on both pointer types: identity beats the function pointer conversion after the
// one conversion function ([over.ics.rank]/3.3).
int pick(int (*)() noexcept) { return 1; }
int pick(int (*)()) { return 2; }

int main() {
  // Outside templates: no specifier, bare, true and false.
  auto none = [] { return 0; };
  auto bare = []() noexcept { return 0; };
  auto yes = []() noexcept(true) { return 0; };
  auto no = []() noexcept(false) { return 0; };
  if (noexcept(none()) || !noexcept(bare()) || !noexcept(yes()) || noexcept(no())) return 1;
  // A condition in the scope of the parameters.
  auto param = [](int x) noexcept(noexcept(x + 1)) { return x; };
  if (!noexcept(param(1))) return 2;
  if (sized<char>() != 1 || sized<int>() != 0) return 3;
  if (same<int, int>() != 1 || same<int, long>() != 0) return 4;
  if (base_of<Derived>() != 1 || base_of<int>() != 0) return 5;
  if (generic<char>() != 1 || generic<int>() != 0) return 6;
  if (Holder<char>::get() != 1 || Holder<int>::get() != 0) return 7;
  if (asserted<char>() + asserted<int>() != 0) return 8;
  if (converted<char>() != 6 || converted<int>() != 6) return 9;
  // The conversion to a pointer to noexcept function ([expr.prim.lambda.closure]/8).
  int (*plain)() noexcept = yes;
  int (*throwing)() = yes;
  if (plain() != 0 || throwing() != 0 || !noexcept(plain()) || !noexcept((+yes)())) return 10;
  if (pick(yes) != 1 || pick(no) != 2) return 11;
  // An init-capture is in scope in the declarator ([basic.scope.lambda]), hiding an outer name.
  char x = 0;
  auto init = [x = 1L]() noexcept(sizeof(x) == sizeof(long)) -> decltype(x) { return x; };
  if (!noexcept(init()) || sizeof(init()) != sizeof(long)) return 12;
  // A condition naming an enclosing generic lambda's parameter, decided per specialization.
  auto outer = [](auto y) {
    auto l = []() noexcept(sizeof(y) == 1) { return 0; };
    return noexcept(l()) ? 1 : 0;
  };
  if (outer('a') != 1 || outer(0) != 0) return 13;
  return x;
}
