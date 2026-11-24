// Deduction considers the direct match first, then the most derived matching base.
// Different parameter pairs must agree ([temp.deduct.call]/4.3, [temp.deduct.call]/5,
// [temp.deduct.type]/2, #1144).
template<class T> struct Box {};
template<> struct Box<long> : Box<int> {};

struct Derived : Box<long> {};
struct IntSide : Box<int> {};
struct Dominated : IntSide, Derived {};
struct CharSide : Box<char> {};
struct Incomparable : Box<int>, CharSide {};

template<class T> T select(const Box<T>&);
template<class T> T select(const Box<T>*);
void select(...);

extern Box<long> exact;
extern Derived derived;
extern Dominated dominated;
extern Incomparable incomparable;
static_assert(__is_same(decltype(select(exact)), long));
static_assert(__is_same(decltype(select(derived)), long));
static_assert(__is_same(decltype(select(dominated)), long));
static_assert(__is_same(decltype(select(incomparable)), void));
static_assert(__is_same(decltype(select(&incomparable)), void));

template<class T> T agree(const Box<T>&, T);
void agree(...);
static_assert(__is_same(decltype(agree(derived, 0L)), long));
static_assert(__is_same(decltype(agree(exact, 0)), void));
static_assert(__is_same(decltype(agree(derived, 0)), void));

// A failed match can deduce an argument before finding a conflict. The next
// base is eligible even when the failed match has the same primary template.
template<class T, class U> struct Pair {};
template<> struct Pair<int, long> : Pair<short, short> {};
struct Layered : Pair<int, long> {};

template<class T> T pair(const Pair<T, T>&);
extern Pair<int, long> mixed;
extern Layered layered;
static_assert(__is_same(decltype(pair(mixed)), short));
static_assert(__is_same(decltype(pair(layered)), short));

// A derived class inside a template argument does not participate in base
// matching. Deducing long here would instantiate NestedResult<long> and fail
// because long has no member type Result.
template<class T> struct Wrap {};
struct WithResult { using Result = int; };
template<class T> struct NestedResult { using Type = typename T::Result; };

template<class T> typename NestedResult<T>::Type nested(const Wrap<Box<T>>&);
void nested(...);
extern Wrap<Box<WithResult>> matching_wrap;
extern Wrap<Derived> wrapped;
static_assert(__is_same(decltype(nested(matching_wrap)), int));
static_assert(__is_same(decltype(nested(wrapped)), void));
