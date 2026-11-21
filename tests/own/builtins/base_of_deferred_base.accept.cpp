template<bool Result>
struct Answer {
    static constexpr bool value = Result;
};

struct B {};
struct D;

template<class T>
struct Query : Answer<__is_base_of(T, D)> {};

static_assert(Query<volatile D>::value);
static_assert(!Query<int>::value);

struct D : B {};

static_assert(Query<B>::value);
