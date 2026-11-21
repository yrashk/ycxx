template<class T> consteval int immediate() { return 7; }
template<class P> int sink(P p = immediate<P>) { return 0; } //! error
int use() { return sink<int(*)()>(); }
