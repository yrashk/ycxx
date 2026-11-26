template <int N>
constexpr int value = N + 1;

template <class T>
struct Layout {
    static constexpr int alignment = value<alignof(T)>;

    struct First {
        char (a)[value<sizeof(T)>];
    };

    struct Second {
        char (a[value<sizeof(T)>]);
    };
};

static_assert(Layout<char>::alignment == 2);
static_assert(sizeof(Layout<char[2]>::First::a) == 3);
static_assert(sizeof(Layout<char[5]>::Second::a) == 6);
