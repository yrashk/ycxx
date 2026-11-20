// [dcl.struct.bind], [dcl.type.decltype]: the initializer runs once, and a named row is an lvalue.
template<class T, class U> struct same { static constexpr bool value = false; };
template<class T> struct same<T, T> { static constexpr bool value = true; };

using Row = int[2];

int main() {
  Row matrix[2] = {{2, 3}, {7, 11}};
  int i = 0;
  auto [x, y](matrix[i++]);
  x = 9;
  if (i != 1 || matrix[0][0] != 2 || y != 3) return 1;

  auto&& [a, b]{matrix[i++]};
  b = 13;
  if (i != 2 || &a != &matrix[1][0] || matrix[1][1] != 13) return 2;

  auto [first, second] = matrix;
  static_assert(same<decltype(second), Row>::value);
  static_assert(same<decltype((second)), Row&>::value);
  first[1] = 4;
  const auto& [left, right] = matrix;
  static_assert(same<decltype(left), const Row>::value);
  const int& original = matrix[1][0];
  if (&right[0] != &original || first[1] != 4 || left[1] != 3) return 3;

  auto&& [p, q] = Row{17, 19};
  const auto& [r, s] = Row{23, 29};
  auto [u, v] = Row{31, 37};
  p += q;
  return p != 36 || r + s != 52 || u + v != 68;
}
