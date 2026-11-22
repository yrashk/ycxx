template<class T>
requires (__is_same(T, int))
constexpr int V = 17;

template<int N>
struct Tag {
    static constexpr int value = N;
};

static_assert(Tag<V<int>>::value == 17);
static_assert(Tag<V<int> >::value == 17);
