template<bool> struct Flag {};
template<bool Result> using bool_constant = Flag<Result>;
struct Later;
template<class T> struct ViaAlias : bool_constant<__is_trivially_copyable(T)> {};
struct Later { int payload; };

Flag<true>* scalar(ViaAlias<int>* value) { return value; }
Flag<true>* qualified(ViaAlias<const int>* value) { return value; }
Flag<false>* reference(ViaAlias<int&>* value) { return value; }
Flag<true>* completed(ViaAlias<Later>* value) { return value; }
