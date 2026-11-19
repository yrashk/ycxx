// #337 (ι): since C++23 a label may precede a declaration ([stmt.label]/1, [stmt.block]/1,
// P2324): the labelled block item is a declaration, including a case label before one.
// EXIT: 0
int constructed = 0;
struct Thing {
  int v;
  Thing(int x) : v(x) { ++constructed; }
};

int loop_to_declaration() {
  int count = 0;
top:
  int value = count * 2;
  ++count;
  if (count < 3) goto top;
  return value;
}

int class_object() {
  constructed = 0;
  int round = 0;
again:
  Thing t(round);
  ++round;
  if (round < 4) goto again;
  return t.v;
}

int case_before_declaration(int v) {
  switch (v) {
    case 1: {
      return 10;
    }
    default:
      int x;
      x = v + 1;
      return x;
  }
}

int main() {
  if (loop_to_declaration() != 4) return 1;
  if (class_object() != 3) return 2;
  if (constructed != 4) return 3;
  if (case_before_declaration(1) != 10) return 4;
  if (case_before_declaration(5) != 6) return 5;
  return 0;
}
