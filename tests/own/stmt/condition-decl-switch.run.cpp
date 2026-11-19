// #337 (ι): a declaration as the condition of a switch ([stmt.switch]/1, /3, [stmt.pre]/4): the
// switch is on its value, and the name is in scope throughout the switch body.
// EXIT: 0
int g(int v) { return v * 2; }

int dispatch(int v) {
  switch (int k = g(v)) {
    case 2: return k + 1;
    case 4: return k + 2;
    default: return -k;
  }
}

enum class Kind { Small, Large };
Kind kind_of(int v) { return v < 10 ? Kind::Small : Kind::Large; }

int on_enum(int v) {
  switch (Kind k = kind_of(v)) {
    case Kind::Small:
      return k == Kind::Small ? 1 : 100;
    case Kind::Large:
      return 2;
  }
  return 0;
}

int main() {
  if (dispatch(1) != 3) return 1;
  if (dispatch(2) != 6) return 2;
  if (dispatch(5) != -10) return 3;
  if (on_enum(3) != 1) return 4;
  if (on_enum(30) != 2) return 5;
  return 0;
}
