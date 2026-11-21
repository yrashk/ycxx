// [temp.deduct.call], [conv.fctptr]
template<class T> T pass_pointer(T (*fn)()) { return fn(); }
template<class T> T pass_reference(T (&fn)()) { return fn(); }
int safe() noexcept { return 7; }
int test() { return pass_pointer(&safe) + pass_reference(safe); }
