// #218: A later class definition cannot complete the class at this earlier query.
struct Value;
constexpr bool observed = __is_trivially_copyable(Value); //! error
struct Value { int payload; };
