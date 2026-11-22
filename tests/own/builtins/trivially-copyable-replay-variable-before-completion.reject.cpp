struct Later;
template<class T> constexpr bool copied = __is_trivially_copyable(T); //! error

constexpr bool observed = copied<Later>;
struct Later { int payload; };
