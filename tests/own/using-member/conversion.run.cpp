// Using-declarations of conversion functions: `using B::operator bool;` makes the base's
// conversion function a member of the derived class for lookup, and a derived conversion function
// to the same type hides it ([namespace.udecl]/1, /11, [class.conv.fct], [over.match.conv]).
// (#1150)
// XFAIL: ycxx conversion functions of a class with a base are not considered, and a
// conversion-function-id is not named in an expression (#339)
struct B {
  operator bool() const { return true; }
  operator int() const { return 7; }
  operator long() const { return 8; }
};
struct D : B {
  using B::operator bool;
  using B::operator int;
  operator int() const { return 9; }   // hides B::operator int
  int self() const { return operator int() + (operator bool() ? 100 : 0); }
};
int main() {
  D d;
  int bad = 0;
  if (!d) bad |= 1;
  if (d.operator bool() != true) bad |= 2;
  if (d.operator int() != 9) bad |= 4;
  int i = d;
  if (i != 9) bad |= 8;
  long l = d.operator long();
  if (l != 8) bad |= 16;
  if (d.self() != 109) bad |= 32;
  bool b = static_cast<bool>(d);
  if (!b) bad |= 64;
  return bad;
}
