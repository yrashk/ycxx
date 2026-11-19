// ι (#298), from the review: the decl-specifiers of a for-range-declaration are type-specifiers
// and `constexpr` ([stmt.pre]/5, [stmt.ranged]); a constexpr one needs a constant initializer.
int a[2] = {1, 2};
int f() { int s = 0; for (extern int x : a) s += x; return s; } //! error
int g() { int s = 0; for (typedef int x : a) s += 1; return s; } //! error
int h() { int s = 0; for (constexpr int x : a) s += x; return s; } //! error
void constexpr_reference() { for (constexpr int& x : a) {} } //! error
int main() { return f() + g() + h(); }
