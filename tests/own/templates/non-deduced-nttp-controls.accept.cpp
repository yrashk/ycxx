// #798 [temp.func.order], [temp.deduct.partial], [temp.deduct.type].
template<int>
struct Identity { using type = long; };

template<int>
struct Index {};

// The explicit constant parameter is absent from the compared types.
template<int I, class T, class U> int independent(T*, U);
template<int I, class T> long independent(T*, T);
static_assert(__is_same(decltype(independent<7>(static_cast<long*>(nullptr), 0L)), long));

// The same parameter is deducible from Index<I> in partial ordering.
template<int I, class T, class U>
int indexed(Index<I>, T*, U, typename Identity<I>::type*);
template<int I, class T>
long indexed(Index<I>, T*, T, typename Identity<I>::type*);
static_assert(__is_same(decltype(indexed<7>(Index<7>{}, static_cast<long*>(nullptr), 0L,
                                           static_cast<long*>(nullptr))), long));

// The defaulted function parameter is outside the types compared for this call.
template<int I, class T> int omitted(T);
template<int I, class T> long omitted(T*, typename Identity<I>::type* = nullptr);
static_assert(__is_same(decltype(omitted<7>(static_cast<long*>(nullptr))), long));

template<class> struct BoundIdentity { using type = long; };
template<class E, E N, class T>
int bound_first(int (&)[N], E, typename BoundIdentity<E>::type*, T);
template<class E, E N, class T>
long bound_first(int (&)[N], E, typename BoundIdentity<E>::type*, T*);
template<class E, E N, class T>
int type_first(E, int (&)[N], typename BoundIdentity<E>::type*, T);
template<class E, E N, class T>
long type_first(E, int (&)[N], typename BoundIdentity<E>::type*, T*);

int bound_values[2];
static_assert(__is_same(decltype(bound_first<int, 2>(bound_values, 0, nullptr, static_cast<int*>(nullptr))), long));
static_assert(__is_same(decltype(type_first<int, 2>(0, bound_values, nullptr, static_cast<int*>(nullptr))), long));
