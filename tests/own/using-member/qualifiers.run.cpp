// Hiding by a derived member needs the same name, parameter-type-list, cv-qualification and
// ref-qualifier ([namespace.udecl]/11, [basic.scope.scope] "corresponding"); otherwise the base's
// function stays a candidate, with an implicit object parameter of the derived class
// ([namespace.udecl]/12, [over.match.funcs.general]/4). (#1150)
struct B {
  int f() { return 1; }
  int g() const { return 2; }
  int h() & { return 3; }
  int h() && { return 4; }
  int i(int) { return 5; }
  int j() const { return 6; }
};
struct D : B {
  using B::f;
  int f() const { return 11; }       // different cv-qualification: both are candidates
  using B::g;
  int g() { return 12; }             // different cv-qualification: both are candidates
  using B::h;
  int h() && { return 14; }          // hides B::h() && only
  using B::i;
  int i(int) const { return 15; }    // different cv-qualification: B::i(int) stays
  using B::j;
  int j() const { return 16; }       // corresponds: hides B::j() const
};
int main() {
  D d;
  const D& c = d;
  int bad = 0;
  if (d.f() != 1 || c.f() != 11) bad |= 1;
  if (d.g() != 12 || c.g() != 2) bad |= 2;
  if (d.h() != 3) bad |= 4;
  if (static_cast<D&&>(d).h() != 14) bad |= 8;
  if (d.i(0) != 5 || c.i(0) != 15) bad |= 16;
  if (d.j() != 16 || c.j() != 16) bad |= 32;
  return bad;
}
