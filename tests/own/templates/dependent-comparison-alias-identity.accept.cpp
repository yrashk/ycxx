// #1548, from the review: an alias template whose expansion waits for its replay keeps its own
// identity while its arguments are dependent ([temp.alias]/2, [temp.over.link]/5), and gives the
// replayed type once they are known: in a return type, a default template argument and a
// non-deduced parameter.
template<bool B, class T, class F> struct cond_ { using type = T; };
template<class T, class F> struct cond_<false, T, F> { using type = F; };
template<class, class> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;
template<class T> concept Small = sizeof(T) < 4;
template<class T> concept Int = __is_same(T, int);

template<class I> using gt = typename cond_<(sizeof(I) > 4), int, long>::type;
template<class I> using lt = typename cond_<(sizeof(I) < 4), int, long>::type;
template<class I> using both = typename cond_<Small<I> && Int<I>, int, long>::type;
template<class I> using either = typename cond_<Small<I> || Int<I>, int, long>::type;

// Distinct function templates, not redefinitions.
template<class T> gt<T> f(T) { return 0; }
template<class T> lt<T> f(T) { return 0; }
template<class T> both<T> g(T) { return 0; }
template<class T> either<T> g(T) { return 0; }
template<class T> gt<T> h(T);
template<class T> gt<T> h(T);  // a redeclaration

static_assert(same<decltype(h('a')), long>);
static_assert(same<decltype(h(1L)), int>);
template<class T> either<T> e(T);
static_assert(same<decltype(e('a')), int>);
static_assert(same<decltype(e(1L)), long>);
template<class T> both<T> b(T);
static_assert(same<decltype(b(1)), long>);

template<class T, class R = gt<T>> constexpr int by_default() { return sizeof(R); }
static_assert(by_default<char>() == sizeof(long));
static_assert(by_default<long>() == sizeof(int));

template<class T> constexpr int param(gt<T>, T) { return 1; }
static_assert(param('a', 'a') == 1);
static_assert(param<char>(1L, 'a') == 1);

template<class T> constexpr int body() { using X = gt<T>; return sizeof(X); }
static_assert(body<char>() == sizeof(long));

// A partial specialization keyed on the alias: non-deduced there, and more specialized.
template<class T, class = void> struct P { static constexpr int v = 0; };
template<class T> struct P<T, gt<T>> { static constexpr int v = 1; };
template<class T, class = void> struct Q { static constexpr int v = 0; };
template<class T> struct Q<T*, gt<T>> { static constexpr int v = 1; };
static_assert(P<long, int>::v == 1 && P<char, long>::v == 1 && P<char, int>::v == 0);
static_assert(Q<long*, int>::v == 1 && Q<long*, long>::v == 0);
