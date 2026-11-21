template<bool> struct Flag {};
struct Later;
template<class T> struct Deferred : Flag<__is_trivially_copyable(T)> {};
template<class T> struct Eager : Flag<__is_trivially_copyable(Later)> {}; //! error

Deferred<Later>* deferred;
Eager<int>* unused;
struct Later { int payload; };
