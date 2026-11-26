// #1244: `decltype(auto)` and `auto...` parameters ([temp.param]/7, [dcl.type.auto.deduct]),
// a placeholder parameter's default, its type as a later default, and partial specializations
// that tell the deduced types apart ([temp.spec.partial.match]).
template<class T, class U> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;

template<decltype(auto) X> struct dw { using type = decltype(X); };
static_assert(same<dw<3L>::type, long>);
constexpr int k = 4;
static_assert(same<dw<k>::type, int>);

template<auto... Xs> struct list { static constexpr int size = sizeof...(Xs); };
static_assert(list<1, 'c', 2u>::size == 3);
static_assert(list<>::size == 0);
static_assert(!same<list<1, 2>, list<1, 2u>>);
static_assert(same<list<1, 'c'>, list<1, 'c'>>);

template<auto X = 7> struct dflt { static constexpr auto value = X; };
static_assert(same<decltype(dflt<>::value), const int>);
static_assert(dflt<>::value == 7 && dflt<'x'>::value == 'x');

template<auto X, class = decltype(X)> struct wrap { static constexpr int k = 0; };
template<auto X> struct wrap<X, int> { static constexpr int k = 1; };
template<auto X> struct wrap<X, char> { static constexpr int k = 2; };
static_assert(wrap<5>::k == 1);
static_assert(wrap<'x'>::k == 2);
static_assert(wrap<5L>::k == 0);

template<auto X, decltype(X) Y> struct pair { static constexpr bool equal = X == Y; };
static_assert(pair<3, 3>::equal && !pair<'a', 'b'>::equal);
