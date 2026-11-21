template<int N>
struct Base {
    static constexpr int value = N;
};

struct Derived : Base<auto{7}> {};

static_assert(__is_base_of(Base<7>, Derived));
static_assert(Derived::value == 7);
