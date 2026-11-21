// XFAIL: clang,gcc Peers support record construction in this trait
struct Record {};
constexpr bool value = __is_trivially_constructible(Record); //! error
