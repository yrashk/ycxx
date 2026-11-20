// [expr.add], [expr.eq]: a stored row-end address in the linked executable (#111).
constexpr int m[2][2] = {{13, 37}, {83, 109}};
extern constexpr const int* row_end = m[0] + 2;
const int* const volatile* saved = &row_end;

int main() {
  const int* p = *saved;
  return p[-1] == 37 && p - m[0] == 2 && p == m[1] ? 0 : 1;
}
