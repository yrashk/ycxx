// [expr.prim.id.unqual], [stmt.return]: an eligible name, including a
// parenthesized name, is an xvalue in a return statement.
// EXIT: 0
struct MoveOnly {
  int value;

  MoveOnly(int n) : value(n) {}
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&& other) : value(other.value) { other.value = 0; }
};

MoveOnly from_local(int value) {
  MoveOnly local(value);
  return (local);
}

MoveOnly from_parameter(MoveOnly parameter) {
  return parameter;
}

MoveOnly from_rvalue_reference(MoveOnly&& parameter) {
  return parameter;
}

MoveOnly& from_reference(MoveOnly& parameter) {
  return parameter;
}

int main() {
  MoveOnly local = from_local(17);
  MoveOnly parameter = from_parameter(MoveOnly(23));
  MoveOnly reference = from_rvalue_reference(MoveOnly(29));
  if (local.value != 17 || parameter.value != 23 || reference.value != 29) return 1;
  MoveOnly& alias = from_reference(local);
  alias.value = 31;
  return local.value != 31;
}
