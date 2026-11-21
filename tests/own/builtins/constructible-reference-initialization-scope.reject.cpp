// XFAIL: clang,gcc This valid init query is outside the current ycxx classifier.
constexpr bool observed = __is_constructible(int&, int&); //! error
