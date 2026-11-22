template<bool> struct Flag {};
struct Later;
template<class T> struct ViaBase : Flag<__is_trivially_copyable(T)> {};
struct Concrete : Flag<__is_trivially_copyable(const int)> {};
struct Later { int payload; };

Flag<true>* scalar(ViaBase<int>* value) { return value; }
Flag<true>* qualified(ViaBase<const int>* value) { return value; }
Flag<false>* reference(ViaBase<int&>* value) { return value; }
Flag<true>* completed(ViaBase<Later>* value) { return value; }
Flag<true>* concrete(Concrete* value) { return value; }
