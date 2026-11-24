// #306 [basic.scope.hiding]/2: in a nested class, an enclosing class's static data member hides
// a member class of the same name; `struct I` still names the class.
struct O {
  struct I { char c[8]; };
  static const int I = 4;
  struct J {
    int f() const { return sizeof(I); }
    int g() const { return sizeof(struct I); }
  };
};

int main() {
  O::J j;
  return (j.f() == sizeof(int) && j.g() == 8) ? 0 : 1;
}
