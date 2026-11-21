// XFAIL: clang,gcc Peers support reference binding in this trait
constexpr bool value = __is_trivially_constructible(int&, int&); //! error
