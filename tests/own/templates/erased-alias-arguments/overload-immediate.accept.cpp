template<class, class> using Ignore = int;
template<class T> struct Nested { using type = typename T::nested; };

template<class T>
using Code = Ignore<typename T::code, typename Nested<T>::type>;

template<class T> Code<T> select(int);
template<class> long select(...);

struct HasBoth { using code = long; using nested = char; };
struct HasNeither {};

template<class T, class U> constexpr bool same_type = false;
template<class T> constexpr bool same_type<T, T> = true;

static_assert(same_type<decltype(select<HasBoth>(0)), int>);
static_assert(same_type<decltype(select<HasNeither>(0)), long>);
