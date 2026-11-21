// XFAIL: clang,gcc This valid init query is outside the current ycxx classifier.
struct Ready { int payload; };
constexpr bool observed = __is_constructible(Ready); //! error
