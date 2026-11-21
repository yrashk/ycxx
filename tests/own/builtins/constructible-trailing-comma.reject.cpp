// A comma must be followed by an argument type-id.
constexpr bool observed = __is_constructible(int,); //! error
