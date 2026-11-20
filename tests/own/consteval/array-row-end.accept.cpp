// [expr.add], [expr.eq]: a stored one-past pointer retains its row (#111).
// XFAIL: gcc GCC 16 rejects negative indexing from the stored end of a nested row.
constexpr int m[2][2] = {{13, 37}, {83, 109}};
constexpr const int* p = m[0] + 2;

static_assert(p[-1] == 37);
static_assert(p - m[0] == 2);
static_assert(p == m[1]);
