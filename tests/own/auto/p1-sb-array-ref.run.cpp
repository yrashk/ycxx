// θ step 1 (#279), spec-only: `auto& [x, y] = arr;` binds the names to the array's own elements, so
// assignment through them modifies the array ([dcl.struct.bind]).
// XFAIL: ycxx structured bindings, #285
int main() {
  int arr[2] = {1, 2};
  auto& [x, y] = arr;
  x = 10;
  y += 20;
  if (arr[0] != 10) return 1;
  if (arr[1] != 22) return 2;
  if (&x != &arr[0] || &y != &arr[1]) return 3;
  arr[0] = 5;
  if (x != 5) return 4;
  return 0;
}
