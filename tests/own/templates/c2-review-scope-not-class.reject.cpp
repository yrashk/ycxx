// ζ c2 (#88), from the step's review: `T::x` with T a non-class type is ill-formed in the
// instantiation ([basic.lookup.qual]/1), not a crash.
template<class T> struct B { int f() { return T::x; } }; //! error
int main() { B<int> b; return b.f(); }
