// [expr.const], [expr.add], [expr.sub], [expr.unary.op]: reading through a pointer or subscript
// that does not designate an element of the array (including the past-the-end position) is
// undefined, so the expression is not constant. Each undefined read is on its marked line.
constexpr const char* text() { return "ab"; }
static_assert(text()[2] == 0 && *(text() + 1) == 'b');
static_assert(text() + 3 != nullptr);
static_assert(text()[3] == 0); //! error
static_assert(*(text() + 3) == 0); //! error
static_assert(text()[-1] == 0); //! error
static_assert("xyz"[4] == 0); //! error
constexpr int past_end() { int a[2]; a[0] = 1; a[1] = 2; return a[2]; } static_assert(past_end()); //! error
constexpr int before() { int a[2]; a[0] = 1; a[1] = 2; return *(a - 1); } static_assert(before()); //! error
constexpr int deref_end() { int x = 5; int* p = &x + 1; return *p; } static_assert(deref_end()); //! error
constexpr int in_bounds() {
  int a[4];
  for (int i = 0; i < 4; ++i)
    a[i] = i * 10;
  int* end = a + 4;
  return *(end - 1);
}
static_assert(in_bounds() == 30);
