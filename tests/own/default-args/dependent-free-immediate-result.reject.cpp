template<class T> consteval int immediate() { return 7; }
template<class P> consteval P leak(P p = immediate<P>) { return p; }
auto use() { return leak<int(*)()>(); } //! error
