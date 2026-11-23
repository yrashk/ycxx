// #218: `std::derived_from`'s shape. The traits' operands are dependent where the concept is
// defined; each atom is evaluated with the arguments substituted when its satisfaction is checked
// ([temp.constr.atomic]), so the classes defined after the concept are complete there.
template<class D, class B>
concept DerivedFrom = __is_base_of(B, D) && __is_convertible(const volatile D*, const volatile B*);

struct A {};
struct E : A {};
struct F : E {};
static_assert(DerivedFrom<E, A>);
static_assert(DerivedFrom<F, A>);
static_assert(DerivedFrom<const F, volatile A>);
static_assert(DerivedFrom<A, A>);
static_assert(!DerivedFrom<A, E>);
static_assert(!DerivedFrom<int, A>);
static_assert(!DerivedFrom<A, int>);

// A private or ambiguous base is still a base ([meta.rel]), but the pointer conversion is
// ill-formed ([conv.ptr]/3), so derived_from is false.
template<class D, class B> concept BaseOf = __is_base_of(B, D);
struct Private : private A {};
struct Left : A {};
struct Right : A {};
struct Both : Left, Right {};
static_assert(BaseOf<Private, A> && !DerivedFrom<Private, A>);
static_assert(BaseOf<Both, A> && !DerivedFrom<Both, A>);
static_assert(DerivedFrom<Both, Left>);
struct Abstract { virtual void f() = 0; };
struct Concrete : Abstract { void f() override {} };
static_assert(DerivedFrom<Concrete, Abstract> && !DerivedFrom<Abstract, Concrete>);

// The same class need not be complete ([meta.rel]); nor does a non-class operand.
struct Incomplete;
static_assert(BaseOf<Incomplete, Incomplete>);
static_assert(!BaseOf<Incomplete, int>);

// A class template specialization is instantiated where the constraint is checked.
template<class T> struct Wrapped : A {};
static_assert(DerivedFrom<Wrapped<int>, A>);
static_assert(!BaseOf<Wrapped<int>, E>);

// A variable template's requires-clause is read again when it is named with arguments; the trait
// is still judged where the constraint is checked, after the class is complete.
template<class T> requires __is_base_of(A, T) && __is_empty(T) constexpr int empty_derived = 1;
struct Late : A {};
static_assert(empty_derived<Late> == 1);

// The other traits that need a complete class wait for satisfaction too.
template<class T, class U> concept Assignable = __is_assignable(T, U);
template<class T, class... Args> concept Constructible = __is_constructible(T, Args...);
template<class T> concept Destructible = __is_nothrow_destructible(T);
template<class T> concept TriviallyCopyable = __is_trivially_copyable(T);
static_assert(Assignable<int&, long> && !Assignable<const int&, int>);
static_assert(Constructible<int, long> && Constructible<double, int> && Constructible<int>);
static_assert(!Constructible<int, long, long>);
static_assert(Destructible<int> && TriviallyCopyable<A>);

int main() {}
