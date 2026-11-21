template<bool> struct Flag {};
struct Later;
template<class T> struct ViaBase : Flag<__is_trivially_copyable(T)> {}; //! error

static_assert(sizeof(ViaBase<Later>) > 0);
struct Later { int payload; };
