struct Later;
template<class T> constexpr bool copied = __is_trivially_copyable(T);
static_assert(__is_trivially_copyable(const int));
struct Later { int payload; };
struct HasWork { ~HasWork() {} };

static_assert(copied<int>);
static_assert(copied<const int>);
static_assert(!copied<int&>);
static_assert(copied<Later>);
static_assert(!copied<HasWork>);
