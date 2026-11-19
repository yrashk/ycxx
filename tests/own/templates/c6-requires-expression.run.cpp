// ζ c6 (#88): a requires-expression's simple, type, compound and nested requirements; a compound
// requirement's return-type-requirement checks decltype((E)) against its type-constraint
// ([expr.prim.req]).
// EXIT: 0
template<class T, class U> concept Same = __is_same(T, U);
template<class T> concept HasType = requires { typename T::type; };
template<class T> concept Sized = requires(const T& t) {
  { t.size() } -> Same<int>;
  requires __is_class(T);
};
template<class T> concept Addable = requires(T a, T b) { a + b; };
struct A { using type = int; int n; int size() const { return n; } };
struct B { long size() const { return 0; } };
struct C { char size() const { return 0; } };
static_assert(HasType<A> && !HasType<int> && Sized<A> && !Sized<B> && !Sized<C> && !Sized<int>);
static_assert(Addable<int> && Addable<int*> == false && !Addable<A>);
template<class T> int count(const T& t) requires Sized<T> { return t.size(); }
template<class T> int count(const T&) { return -1; }
int main() {
  A a{5};
  B b;
  return count(a) == 5 && count(b) == -1 && requires { a.n; } ? 0 : 1;
}
