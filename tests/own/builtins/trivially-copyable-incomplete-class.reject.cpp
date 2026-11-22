// #218: An incomplete class operand is outside the trait domain.
struct Incomplete;
constexpr bool observed = __is_trivially_copyable(Incomplete); //! error
