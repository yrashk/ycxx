// XFAIL: clang,gcc This valid init query is outside the current ycxx classifier.
using Pair = int[2];
constexpr bool observed = __is_constructible(Pair, int, int); //! error
