// XFAIL: clang,gcc Peers evaluate complete class sources in this trait
struct Source {};
constexpr bool value = __is_trivially_constructible(int, Source); //! error
