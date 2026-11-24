// #218: a dependent `__is_base_of` in a requires-clause, a trailing requires-clause and a
// nested-requirement is evaluated where satisfaction is checked ([temp.constr.atomic],
// [expr.prim.req.nested]), after the classes it names are complete.
// XFAIL: gcc GCC 16 rejects a type-trait built-in in a function template's signature (a vendor extension it does not mangle; Clang accepts).
struct Base {};

template<class T> requires __is_base_of(Base, T) int pick(T*) { return 1; }
int pick(...) { return 2; }

template<class T> int trailing(T*) requires __is_base_of(Base, T) { return 3; }
int trailing(...) { return 4; }

template<class T> concept Nested = requires { requires __is_base_of(Base, T); };
template<class T> requires Nested<T> struct Only { static constexpr int value = 5; };

struct Derived : Base {};
struct Other {};
static_assert(Nested<Derived> && !Nested<Other> && !Nested<int>);

int main() {
  if (pick(static_cast<Derived*>(nullptr)) != 1) return 1;
  if (pick(static_cast<Other*>(nullptr)) != 2) return 2;
  if (trailing(static_cast<Derived*>(nullptr)) != 3) return 3;
  if (trailing(static_cast<Other*>(nullptr)) != 4) return 4;
  return Only<Derived>::value == 5 ? 0 : 5;
}
