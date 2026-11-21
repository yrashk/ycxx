// XFAIL: clang,gcc Peers support class conversion after pack replay
struct Conversion {
    operator double() noexcept;
};

template<bool>
struct Verdict {};

template<class... Sources>
struct Derived : Verdict<__is_nothrow_constructible(double, Sources...)> {}; //! error

constexpr auto instance_size = sizeof(Derived<Conversion>);
