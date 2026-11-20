// [conv.array], [expr.const]: typed constant addresses of a named array's row and element (#111).
constexpr int values[2][3] = {{9, 19, 29}, {21, 23, 25}};
constexpr const int* inside = &values[1][1];
constexpr const int (*row)[3] = values + 1;
static_assert(*inside == 23);
static_assert((*row)[1] == 23);
int main() {
  const int* const* slot = &inside;
  return **slot == 23 ? 0 : 1;
}
