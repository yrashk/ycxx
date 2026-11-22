// #218: a dependent `__is_base_of` in a function template body asks for Derived's completeness
// when the body is instantiated ([meta.rel]/2); a class never completed is an error there.
struct B {};
struct Never;
template<class T> bool f() { return __is_base_of(B, T); } //! error
bool use() { return f<Never>(); }
