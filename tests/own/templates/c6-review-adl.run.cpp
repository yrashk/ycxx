// ζ c6 (#88), from the step's review: an unqualified call in an atomic constraint that depends on
// a template parameter also finds, by argument-dependent lookup, the functions declared before the
// point where the constraint is checked ([temp.dep.candidate], [temp.constr.atomic]).
// EXIT: 0
struct A {};
namespace N { struct B {}; }
template<class T> concept Addable = requires(T t) { add(t); };
template<class T> concept Sub = sub((T*)nullptr) > 0;
int add(A*) { return 0; }
namespace N { int add(B*) { return 0; } constexpr int sub(B*) { return 1; } }
template<class T> int f(T) requires Addable<T> { return 1; }
template<class T> int f(T) { return 2; }
static_assert(Sub<N::B> && !Sub<A>);
int main() { return f((A*)0) == 1 && f((N::B*)0) == 1 && f(1) == 2 ? 0 : 1; }
