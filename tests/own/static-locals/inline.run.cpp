// ι (#291): a static block variable in an inline function, or in a member function defined in
// its class (implicitly inline, [class.mfct]/1), is one variable, initialized once like any
// other ([dcl.inline], [stmt.dcl]/3).
// EXIT: 0
int inits = 0;

int make() {
  ++inits;
  return 1;
}

inline int bump() {
  static int n = make();
  return n++;
}

inline int* storage() {
  static int s = 9;
  return &s;
}

struct S {
  int get() {
    static int g = make() * 30;
    return g++;
  }
};

int main() {
  if (bump() != 1) return 1;
  if (bump() != 2) return 2;
  if (inits != 1) return 3;
  int* (*fp)() = storage;
  if (fp() != storage()) return 4;
  *fp() = 11;
  if (*storage() != 11) return 5;
  S s;
  if (s.get() != 30) return 6;
  if (S().get() != 31) return 7;
  if (inits != 2) return 8;
  return 0;
}
