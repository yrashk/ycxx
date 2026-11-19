// [expr.const], [expr.unary.op], from #336's review: negating a NaN or taking its magnitude makes
// no NaN that was not there, so it is a constant expression (only arithmetic producing a NaN is
// not: const-nan.reject.cpp); a namespace-scope variable so initialized is constant-initialized.
constexpr double nan = __builtin_nan("");
constexpr double a = -nan;
constexpr double b = __builtin_fabs(-nan);
constexpr float c = -__builtin_nanf("1");
static_assert(a != a && b != b && c != c);
double g = __builtin_fabs(__builtin_nan(""));
int main() { return g == g; }
