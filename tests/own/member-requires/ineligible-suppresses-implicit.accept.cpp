// #1417 [class.default.ctor], [class.copy.ctor], [class.copy.assign], [special]: a
// user-declared constructor or assignment operator whose constraints are not satisfied is
// still user-declared, so it suppresses the implicit declaration it would otherwise replace;
// the class then has no usable member of that kind.
template<class T> concept Never = sizeof(T) == 0;
template<class T> concept DefaultConstructible = requires { T(); };
template<class T> concept CopyConstructible = requires(const T& t) { T(t); };
template<class T> concept CopyAssignable = requires(T& a, const T& b) { a = b; };

template<class T>
struct NoDefault {
  T v;
  NoDefault() requires Never<T> = default;
  NoDefault(int x) : v(x) {}
};

template<class T>
struct NoCopy {
  T v;
  NoCopy() = default;
  NoCopy(const NoCopy&) requires Never<T> = default;
};

template<class T>
struct NoAssign {
  T v;
  NoAssign& operator=(const NoAssign&) requires Never<T> = default;
};

template<class T>
struct Unconstrained {
  T v;
};

static_assert(!DefaultConstructible<NoDefault<int>>);
static_assert(!CopyConstructible<NoCopy<int>>);
static_assert(!CopyAssignable<NoAssign<int>>);

static_assert(DefaultConstructible<Unconstrained<int>>);
static_assert(CopyConstructible<Unconstrained<int>>);
static_assert(CopyAssignable<Unconstrained<int>>);
