// [expr.const], [expr.add]: the row end and its neighbor retain separate arithmetic domains.
// XFAIL: gcc GCC 16 accepts the read and subtraction across the shared byte address.
constexpr int m[2][2] = {{13, 37}, {83, 109}};
constexpr const int* p = m[0] + 2;
constexpr const int* q = m[1];

constexpr int past_end = *p;  //! error
constexpr auto cross_row = p - q;  //! error
