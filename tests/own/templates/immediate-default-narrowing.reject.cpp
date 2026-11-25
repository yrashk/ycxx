// [temp.arg.nontype], [expr.const]
consteval int divide(int n) { return 12 / n; }
// GCC reports the use and Clang reports the default.
template<int N, unsigned A = divide(N)> struct Quotient; Quotient<-3>* bad; //! error
