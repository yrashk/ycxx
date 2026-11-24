// A template argument that can be a type-id is one ([temp.arg.general]/2). `int(…)` can be one
// only as a function type; a functional cast ([expr.type.conv]) otherwise, including when its
// operand is a member of a dependent class without `typename` ([temp.res.general]/4) (#308).
template<bool B> struct BC { static constexpr bool value = B; };
template<int N> struct IC { static constexpr int value = N; };
template<class T> struct Ty { using type = T; };

struct F { static constexpr bool value = true; static constexpr int n = 3; using type = long; };

// Functional casts.
static_assert(BC<bool(3)>::value);
static_assert(IC<int(3) + 1>::value == 4);
static_assert(BC<bool(0) || true>::value);
static_assert(!BC<bool(1) && false>::value);
template<int N> struct FromParam : BC<bool(N)> {};
static_assert(FromParam<2>::value && !FromParam<0>::value);
template<class T> struct Member : BC<bool(T::value)> {};
template<class T> struct MemberOr : BC<bool(T::value) || false> {};
template<class T> struct MemberPlus : IC<int(T::n) + 1> {};
static_assert(Member<F>::value && MemberOr<F>::value && MemberPlus<F>::value == 4);

// Type-ids.
static_assert(__is_function(Ty<int()>::type) && __is_function(Ty<void(int, char)>::type));
static_assert(__is_same(Ty<bool(F)>::type, bool(F)) && __is_same(Ty<void(...)>::type, void(...)));
static_assert(__is_same(Ty<void(int) noexcept>::type, void(int) noexcept));
static_assert(__is_same(Ty<int (*)(int)>::type, int (*)(int)) && __is_same(Ty<int (&)[3]>::type, int (&)[3]));
static_assert(__is_same(Ty<void() &>::type, void() &) && __is_same(Ty<void() const>::type, void() const));
static_assert(__is_same(Ty<int (F::*)()>::type, int (F::*)()));
template<class T> struct Param : Ty<void(typename T::type)> {};
static_assert(__is_same(Param<F>::type, void(long)));
static_assert(__is_same(Ty<int(__attribute__((unused)) int)>::type, int(int)));
