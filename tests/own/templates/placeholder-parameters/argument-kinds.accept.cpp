// #1244: an `auto` parameter deduces any structural type from its argument ([temp.param]/7,
// [temp.arg.nontype]/1): an enumeration, an object or function pointer (after the
// function-to-pointer conversion), `std::nullptr_t`, and a structural class, which is also
// deduced through a specialization ([temp.deduct.type]/8); and `std::size_t` from an array bound
// ([temp.deduct.type], the example with `f(int (&a)[i])`).
// XFAIL: gcc GCC 16 does not deduce std::size_t for an auto parameter from an array bound
template<class T, class U> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;
template<auto X> struct cw { static constexpr auto value = X; using type = decltype(X); };
struct P { int x, y; };
enum E : short { e1 = 1 };
int g;
int fn(int a) { return a; }
static_assert(same<cw<e1>::type, E>);
static_assert(cw<P{1, 2}>::value.y == 2);
static_assert(same<cw<P{1, 2}>::type, P>);
static_assert(same<cw<P{1, 2}>, cw<P{1, 2}>> && !same<cw<P{1, 2}>, cw<P{2, 1}>>);
static_assert(same<cw<&g>::type, int*>);
static_assert(cw<&g>::value == &g);
static_assert(same<cw<nullptr>::type, decltype(nullptr)>);
static_assert(same<cw<&fn>::type, int (*)(int)>);
static_assert(same<cw<fn>::type, int (*)(int)>);
template<auto V> constexpr int y_of(cw<V>) { return V.y; }
static_assert(y_of(cw<P{1, 9}>{}) == 9);
template<auto N> constexpr auto bound(int (&)[N]) { return N; }
int arr[5];
static_assert(bound(arr) == 5 && same<decltype(bound(arr)), decltype(sizeof(int))>);

// Class-type elements of an `auto...` pack are deduced too.
template<auto... Xs> struct list {};
template<auto... Xs> constexpr int count(list<Xs...>) { return sizeof...(Xs); }
static_assert(count(list<P{1, 2}>{}) == 1 && count(list<P{1, 2}, 4, 'c'>{}) == 3);
template<class T> struct is_list { static constexpr int k = 0; };
template<auto... Xs> struct is_list<list<Xs...>> { static constexpr int k = 1; };
static_assert(is_list<list<P{1, 2}, 4>>::k == 1);
