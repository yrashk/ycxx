template<class T> consteval int immediate() { return 7; }
template<class P> consteval int sink(P p = immediate<P>) { return 8; }
static_assert(sink<int(*)()>() == 8);
