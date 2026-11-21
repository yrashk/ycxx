// XFAIL: ycxx array allocation and deallocation during constant evaluation (#293)
// [expr.const], [expr.new], [expr.delete]: use and release the array in the same evaluation.
constexpr int round_trip(unsigned n) {
  int* p = new int[n + 1]();
  p[n] = 5;
  int result = p[0] + p[n];
  delete[] p;
  return result;
}

static_assert(round_trip(2) == 5);
