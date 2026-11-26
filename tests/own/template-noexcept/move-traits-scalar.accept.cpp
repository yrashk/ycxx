// #218, [except.spec], [temp.inst], [expr.unary.noexcept]
// The declarations of two specializations can have different exception specifications.
template<class T>
void move_between(T&, T&) noexcept(
    __is_nothrow_constructible(T, T&&) && __is_nothrow_assignable(T&, T&&));

extern int first, second;
extern const int fixed_first, fixed_second;

static_assert(noexcept(move_between(first, second)));
static_assert(!noexcept(move_between(fixed_first, fixed_second)));
static_assert(noexcept(move_between(first, second)));

// The same scalar operands work in direct queries and existing template contexts.
static_assert(__is_nothrow_constructible(int, int&&));
static_assert(__is_nothrow_constructible(const int, const int&&));
static_assert(__is_nothrow_assignable(int&, int&&));
static_assert(!__is_nothrow_assignable(const int&, const int&&));

template<class Target, class... Sources>
constexpr bool nothrow_constructible = __is_nothrow_constructible(Target, Sources...);

template<class Target, class Source>
constexpr bool nothrow_assignable = __is_nothrow_assignable(Target, Source);

static_assert(nothrow_constructible<int, int&&>);
static_assert(!nothrow_constructible<int, int&&, long>);
static_assert(nothrow_assignable<int&, int&&>);
static_assert(!nothrow_assignable<const int&, const int&&>);
