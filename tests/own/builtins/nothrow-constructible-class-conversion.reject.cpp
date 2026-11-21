// XFAIL: clang,gcc Peers support class conversion in this trait
struct Conversion {
    operator double() noexcept;
};
constexpr bool value = __is_nothrow_constructible(double, Conversion); //! error
