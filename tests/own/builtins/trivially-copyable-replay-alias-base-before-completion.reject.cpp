template<bool> struct Flag {};
template<bool Result> using bool_constant = Flag<Result>;
struct Later;
template<class T> struct ViaAlias : bool_constant<__is_trivially_copyable(T)> {}; //! error

static_assert(sizeof(ViaAlias<Later>) > 0);
struct Later { int payload; };
