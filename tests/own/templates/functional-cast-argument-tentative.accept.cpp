// A template argument is a type-id only if it parses as one ([temp.arg.general]/2,
// [dcl.ambig.res]/2). `T(…)` with T a simple-type-specifier, a typedef-name or a qualified type
// name, also after `typename`, is otherwise a functional cast ([expr.type.conv]) (#1190, #810).
template<bool B> struct BC { static constexpr bool value = B; };
template<int N> struct IC { static constexpr int value = N; };
template<class T> struct Ty { using type = T; };
struct S {};
enum class E : int {};
constexpr int n = 1;
int g;
namespace ns {
using I = int;
struct P { using J = int; };
}
using U = int;
using B = bool;
using u16 = unsigned short;
template<class C, class I, C E> struct Traits { static constexpr I eof = I(E); };

// Expressions: the parentheses hold no parameter-declaration-clause or abstract declarator.
static_assert(BC<bool(&g)>::value);
static_assert(IC<int((n))>::value == 1);
static_assert(IC<int(E{2})>::value == 2);
static_assert(IC<int(int{2})>::value == 2);
static_assert(IC<int(auto(3))>::value == 3);
static_assert(IC<int(*&n == 1)>::value == 1);
template<class T> struct X : IC<int(T{2})> {};
static_assert(X<E>::value == 2);

// The same through a typedef-name or a qualified name.
static_assert(IC<U(3)>::value == 3);
static_assert(IC<U(3) + 1>::value == 4);
static_assert(IC<U((n))>::value == 1);
static_assert(BC<B(&g)>::value);
static_assert(IC<U(E{4})>::value == 4);
static_assert(IC<ns::I(4)>::value == 4);
static_assert(IC<ns::P::J(5)>::value == 5);
static_assert(Traits<char16_t, u16, u16(0xFFFF)>::eof == 0xFFFF);
template<class T> struct Y : IC<U(sizeof(T))> {};
static_assert(Y<int>::value == sizeof(int));
enum En { e0, e5 = 5 };
template<En V> constexpr int h() { return V; }
static_assert(h<En(5)>() == 5);

// After `typename` (#810).
struct H { using type = int; };
static_assert(IC<typename ns::I(2)>::value == 2);
static_assert(IC<typename ns::I{1}>::value == 1);
static_assert(IC<typename ns::P::J(6)>::value == 6);
template<class T> struct D : IC<typename T::type(3)> {};
template<class T> constexpr int db() { return IC<typename T::type{4}>::value; }
template<class T> constexpr int dp() { return IC<typename T::type(5)>::value; }
static_assert(D<H>::value == 3 && db<H>() == 4 && dp<H>() == 5);

// Class names, class template-ids, `->` after the parentheses, `>>` and pack expansions.
template<class T> struct W { int v = 3; constexpr W(int) {} };
struct M { int m = 9; };
using MP = const M*;
constexpr M mo{};
static_assert(IC<W<int>(3).v>::value == 3);
static_assert(IC<MP(MP(&mo))->m>::value == 9);
static_assert(IC<U(Ty<IC<1>>::type::value)>::value == 1);

// Type-ids keep their precedence.
static_assert(__is_same(Ty<int(int)>::type, int(int)));
static_assert(__is_same(Ty<int (&)[3]>::type, int (&)[3]));
static_assert(__is_same(Ty<int (&&)[2]>::type, int (&&)[2]));
static_assert(__is_same(Ty<int (*const)(int)>::type, int (*const)(int)));
static_assert(__is_same(Ty<int(*(int))>::type, int*(int)));
static_assert(__is_same(Ty<int(U)>::type, int(int)));
static_assert(__is_same(Ty<bool(S)>::type, bool(S)));
static_assert(__is_same(Ty<U(int)>::type, int(int)));
static_assert(__is_same(Ty<U (&)[3]>::type, int (&)[3]));
static_assert(__is_same(Ty<U(S, U, ...)>::type, int(S, int, ...)));
static_assert(__is_same(Ty<ns::I (*)(U)>::type, int (*)(int)));
static_assert(__is_same(Ty<ns::P::J()>::type, int()));
static_assert(__is_same(Ty<S (&)[2]>::type, S (&)[2]) && __is_same(Ty<S(S)>::type, S(S)));
static_assert(__is_same(Ty<W<int>(int)>::type, W<int>(int)));
static_assert(__is_same(Ty<W<W<int>> (&)[2]>::type, W<W<int>> (&)[2]));
static_assert(__is_same(Ty<Ty<int (&)[3]>>::type, Ty<int (&)[3]>));
template<class...> struct TL {};
template<class... Ts> struct Fns { using type = TL<U (*)(Ts)..., U(Ts)...>; };
static_assert(__is_same(Fns<char, S>::type, TL<int (*)(char), int (*)(S), int(char), int(S)>));
template<class T> struct Z : Ty<U(T)> {};
static_assert(__is_same(Z<S>::type, int(S)));
static_assert(__is_same(Ty<typename ns::I(int)>::type, int(int)));
static_assert(__is_same(Ty<typename ns::I (&)[3]>::type, int (&)[3]));
template<class T> struct TZ : Ty<typename T::type(T)> {};
static_assert(__is_same(TZ<H>::type, int(H)));
