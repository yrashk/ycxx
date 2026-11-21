template<bool Result>
struct Answer {
    static constexpr bool value = Result;
};

struct B {};
struct D;

template<class T>
struct Query : Answer<__is_base_of(B, D)> {}; //! error

struct D : B {};

static_assert(Query<int>::value);
