// #199, [expr.reinterpret.cast]: the destination integer must distinguish all representations
// of the source pointer type. Even a known null pointer requires a sufficiently wide type.
unsigned int narrow(int* p) { return reinterpret_cast<unsigned int>(p); } //! error
bool to_bool(const volatile void* p) { return reinterpret_cast<bool>(p); } //! error
unsigned int null_bits() { return reinterpret_cast<unsigned int>(static_cast<int*>(nullptr)); } //! error
