#include "abi.hpp"

int main() {
  int one_value = 10;
  OneRef one{one_value};
  if (direct_one(one, &one, 3) != 13 || one_value != 13) return 1;

  int left = 11;
  int right = 29;
  TwoRefs two{left, right};
  if (direct_two(two, &two, 7) != 4 || left != 18 || right != 22) return 2;

  // AMD64 psABI §3.2.3. With one INTEGER register left, both eightbytes go on the stack.
  // The trailing scalar still uses the remaining register.
  left = 5;
  right = 41;
  Rollback indirect = rollback_target();
  if (indirect(2, 3, 5, 7, 11, two, 23) != 88 || left != 28 || right != 30) return 3;

  if (int status = ref_captures(true)) return 10 + status;
  if (int status = ref_captures(false)) return 20 + status;
  if (int status = closure_parameter_objects()) return 30 + status;
  if (int status = value_captures()) return 40 + status;
  return 0;
}
