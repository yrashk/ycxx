// ζ f1 (#88), from the step's review: instantiations that nest without end through constant
// evaluation reach the implementation's limit ([temp.inst]/16) and are diagnosed, not a crash.
template<int N> constexpr int f() { constexpr int k = f<N + 1>(); return k; } //! error
static_assert(f<0>() == 0);
