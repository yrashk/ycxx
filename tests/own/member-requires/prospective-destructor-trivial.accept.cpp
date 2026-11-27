// #1417 [class.dtor], [class.prop], [special]: the selected prospective destructor decides
// whether the class is trivially destructible and trivially copyable; the other prospective
// destructors do not matter.
// XFAIL: ycxx type-trait built-ins do not classify class template specializations yet (#218)
template<class T> concept Trivial = __is_trivially_destructible(T);

struct Owner { ~Owner() {} };

template<class T>
struct Opt {
  T payload;
  ~Opt() requires Trivial<T> = default;
  ~Opt() requires (!Trivial<T>) {}
};

static_assert(__is_trivially_destructible(Opt<int>));
static_assert(__is_trivially_copyable(Opt<int>));
static_assert(!__is_trivially_destructible(Opt<Owner>));
static_assert(!__is_trivially_copyable(Opt<Owner>));
