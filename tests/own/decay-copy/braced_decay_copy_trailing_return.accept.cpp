template<int N>
struct Tag {
    static constexpr int value = N;
};

auto f() -> Tag<auto{7}> {
    return {};
}

static_assert(__is_same(decltype(f()), Tag<7>));
static_assert(decltype(f())::value == 7);

constexpr auto placeholder_return() -> auto {
    return 19;
}

static_assert(__is_same(decltype(placeholder_return()), int));
static_assert(placeholder_return() == 19);
