// [conv.fpint]/1, [expr.const]: converting a floating value whose truncated value the integer type
// cannot represent has undefined behavior, so it is not a core constant expression.
constexpr double big() { return 1e10; }
constexpr double minus_one() { return -1.0; }
constexpr double inf = __builtin_huge_val();
constexpr int a = big(); //! error
constexpr unsigned b = minus_one(); //! error
constexpr unsigned char c = 256.0; //! error
constexpr long long d = 1e19; //! error
constexpr int e = inf; //! error
constexpr int f = static_cast<int>(2147483648.0); //! error
constexpr unsigned ok = -0.99;
constexpr int ok2 = 2147483647.75;
static_assert(ok == 0u && ok2 == 2147483647);
