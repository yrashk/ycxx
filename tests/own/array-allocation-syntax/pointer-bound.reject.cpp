// [expr.new]: the bound must implicitly convert to size_t.
int* pointer_bound(int* p) { return new int[p]; } //! error
