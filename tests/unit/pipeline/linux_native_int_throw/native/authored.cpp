// [expr.throw] Lightweight frames with no active local cleanup.
extern "C" int hosted_operand(int mode);

extern "C" int authored_choice(int value) {
  if (value == 0) return 41;
  const int payload = value;
  throw payload;
}

extern "C" int authored_operand(int mode) {
  throw hosted_operand(mode);
}
