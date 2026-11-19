// ι (#291): jumping past the declaration of a static block variable with a dynamic initializer
// is well-formed ([stmt.dcl]/2 restricts only automatic variables); the skipped variable is not
// initialized then, and is initialized the first time control does pass through its
// declaration ([stmt.dcl]/3).
// EXIT: 0
int inits = 0;

int make(int v) {
  ++inits;
  return v;
}

int f(bool skip) {
  int r = 0;
  if (skip) goto done;
  {
    static int s = make(5);
    r = ++s;
  }
done:
  return r;
}

int backward(int rounds) {
  int i = 0;
again:
  static int t = make(100);
  ++t;
  if (++i < rounds) goto again;
  return t;
}

int main() {
  if (f(true) != 0) return 1;
  if (f(true) != 0) return 2;
  if (inits != 0) return 3;
  if (f(false) != 6) return 4;
  if (inits != 1) return 5;
  if (f(true) != 0) return 6;
  if (f(false) != 7) return 7;
  if (inits != 1) return 8;
  // Re-passing the declaration through a backward goto does not initialize it again.
  if (backward(3) != 103) return 9;
  if (inits != 2) return 10;
  if (backward(2) != 105) return 11;
  if (inits != 2) return 12;
  return 0;
}
