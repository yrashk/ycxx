// XFAIL: clang,gcc This valid init query is outside the current ycxx classifier.
struct Incomplete;
constexpr bool observed = __is_constructible(void*, Incomplete*); //! error
