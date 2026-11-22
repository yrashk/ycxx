// #218: The ultimate class element must be complete even when the outer bound is unknown.
struct Incomplete;
constexpr bool observed = __is_trivially_copyable(Incomplete[][2]); //! error
