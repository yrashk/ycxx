// [expr.const]: conversion from void* to an object pointer requires an actual object or null.
// XFAIL: clang,gcc Clang 23 and GCC 16 accept the one-past cast despite the draft's similar-object rule.
constexpr int m[2][2] = {{13, 37}, {83, 109}};
constexpr const int* row_end = m[0] + 2;
constexpr const void* erased = row_end;

constexpr const int* element = static_cast<const int*>(erased);  //! error
