// Local classes (#117), spec-only: a member function of a local class may use a static local
// variable of the enclosing function, which has static storage duration and is not a local
// entity ([class.local], [basic.def.odr], [basic.stc.static]).
int tick() {
  static int counter = 0;
  struct Bumper {
    static int bump() { return ++counter; }
    int bump_by(int n) const {
      counter += n;
      return counter;
    }
  };
  Bumper b;
  Bumper::bump();
  return b.bump_by(10);
}

int main() {
  if (tick() != 11) return 1;
  if (tick() != 22) return 2;
  return 0;
}
