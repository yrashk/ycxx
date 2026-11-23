// #1065 GNU __builtin_constant_p inspects an operand without evaluating it.
// EXIT: 0
extern int unavailable;

int main() {
  int value = 3;
  if (__builtin_constant_p(unavailable) != 0) return 1;
  if (__builtin_constant_p(++value) != 0) return 2;
  return value == 3 ? 0 : 3;
}
