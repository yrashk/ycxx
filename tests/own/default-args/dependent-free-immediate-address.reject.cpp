template<class T> consteval int immediate() { return 7; }
template<class P> int sink(P p = immediate<P>) { return 0; } int use() { return sink<int(*)()>(); } //! error
