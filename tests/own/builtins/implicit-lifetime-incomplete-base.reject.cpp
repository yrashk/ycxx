// #1053 [meta.unary.prop], [temp.arg.nontype].
struct Pending;

template<bool> struct Flag {};
template<class T> struct Lifetime : Flag<__builtin_is_implicit_lifetime(T)> {}; //! error
struct Demand : Lifetime<const Pending> {};
