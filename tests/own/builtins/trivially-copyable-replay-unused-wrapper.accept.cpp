template<bool> struct Flag {};
template<class T> struct Check : Flag<__is_trivially_copyable(T)> {};
struct Forward;
Check<Forward>* deferred;
