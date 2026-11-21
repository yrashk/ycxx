template<bool Result>
struct Answer {
    static constexpr bool value = Result;
};

struct Empty;
struct Nonempty;

template<class T>
struct Query : Answer<__is_empty(T)> {};

struct Empty {};
struct Nonempty {
    int value;
};

static_assert(Query<Empty>::value);
static_assert(!Query<Nonempty>::value);
