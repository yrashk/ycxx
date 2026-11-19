// [dcl.init.list] (narrowing conversion): not narrowing are a floating conversion to equal or
// greater rank, one to lower rank from a constant with a finite result that did not overflow (an
// inexact value is fine), and integer to floating from a constant that fits and converts back to
// its value. (Infinity to float, non-finite on both sides, is not narrowing either, but GCC 16
// rejects it, so it is left out.)
double d{1.5f};
long double ld{1.5};
long double ld2{1.5f};
float f{0.1};
float f2{1e-50}; // underflows to zero but does not overflow
constexpr double dc = 3.25;
float f3{dc};
float f4{0.1L};
constexpr int n = 16777216;
float f5{n};
float f6{16777218};
double d2{9007199254740992LL};
double d3{-2147483647 - 1};
double d4{'x'};
float f7{true};
struct P { float x; double y; };
P p{0.5, 1};
float arr[3]{1, 2.5, 3.0L};
static_assert(sizeof(arr) == 3 * sizeof(float));
