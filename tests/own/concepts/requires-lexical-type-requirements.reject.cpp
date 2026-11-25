template<class T>
struct Later {
    static_assert(T::allow_later, "later type requirement reached"); //! error {{later type requirement reached}}
    using type = int;
};

template<class T>
concept HasRequiredTypes = requires {
    typename T::element;
    typename Later<T>::type;
};

struct Reached {
    using element = int;
    static constexpr bool allow_later = false;
};

constexpr bool reached = HasRequiredTypes<Reached>;
