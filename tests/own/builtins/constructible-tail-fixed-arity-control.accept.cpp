template<bool> struct Flag {};

template<class T>
struct Copyability : Flag<__is_trivially_copyable(T)> {};

Flag<true>* scalar(Copyability<int>* p) { return p; }
Flag<false>* reference(Copyability<int&>* p) { return p; }
static_assert(__is_same(int, int));
