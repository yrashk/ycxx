template<class T>
struct Later {
    static_assert(T::allow_later, "later type requirement reached");
    using type = int;
};

template<class T>
concept HasRequiredTypes = requires {
    typename T::element;
    typename Later<T>::type;
};

struct MissingElement {
    static constexpr bool allow_later = false;
};

struct Allowed {
    using element = int;
    static constexpr bool allow_later = true;
};

static_assert(!HasRequiredTypes<MissingElement>);
static_assert(HasRequiredTypes<Allowed>);
