struct Later;
using Row = Later[2];
constexpr bool observed = __is_constructible(int, double, Row); //! error
struct Later { int payload; };
