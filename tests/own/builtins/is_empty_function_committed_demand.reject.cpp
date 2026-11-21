struct Empty;

template<class T>
constexpr bool checked = __is_empty(T); //! error

bool demand() {
    return checked<Empty>;
}

struct Empty {};

static_assert(__is_empty(Empty));
