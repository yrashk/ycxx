// [temp.arg.general], [temp.arg.nontype], [expr.const]
// GCC reports the operation and Clang reports the default.
consteval int divide(int n) { return 12 / n; } template<int N, unsigned A = divide(N)> struct Quotient; Quotient<0>* bad; //! error
