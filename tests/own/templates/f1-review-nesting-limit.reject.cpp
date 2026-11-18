// ζ f1 (#88), from the step's review: instantiations that nest without end through constant
// evaluation reach the implementation's limit ([temp.inst]/16) and are diagnosed, not a crash.
// A small limit for every compiler: at the default depth Clang overflows its stack on CI.
// FLAGS: -ftemplate-depth=32
template<int N> constexpr int f() { constexpr int k = f<N + 1>(); return k; } //! error
static_assert(f<0>() == 0);
