// #218: The ultimate class element of a bounded array must be complete.
struct Incomplete;
constexpr bool observed = __is_trivially_copyable(Incomplete[2]); //! error
