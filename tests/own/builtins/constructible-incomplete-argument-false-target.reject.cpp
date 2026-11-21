struct Later;
using Function = int(int);
constexpr bool void_observed = __is_constructible(const volatile void, int, Later); //! error
constexpr bool function_observed = __is_constructible(Function, Later); //! error
struct Later { int payload; };
