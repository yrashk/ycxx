extern "C" int chosen(int);
extern "C" int transit(int);

static int calls;
static int guards;
static int inner_destroyed;
static int outer_destroyed;
static int last_inner;

struct Guard {
  ~Guard() { ++guards; }
};

struct Inner {
  int code;
  ~Inner() {
    last_inner = code;
    ++inner_destroyed;
  }
};

struct Outer {
  int code;
  ~Outer() { ++outer_destroyed; }
};

extern "C" int peer(int x) {
  ++calls;
  Guard guard;
  if (x < 0) throw Inner{-x};
  return 100 + x;
}

static bool counts(int invoked, int unwound, int dead_inner, int dead_outer) {
  return calls == invoked && guards == unwound &&
         inner_destroyed == dead_inner && outer_destroyed == dead_outer;
}

int main() {
  if (chosen(7) != 107 || !counts(1, 1, 0, 0)) return 1;
  if (chosen(-9) != 47) return 2;
  if (!counts(2, 2, 1, 0) || last_inner != 9) return 3;

  const Outer* original = nullptr;
  try {
    try {
      throw Outer{89};
    } catch (const Outer& outer) {
      original = &outer;
      if (chosen(13) != 113 || !counts(3, 3, 1, 0)) return 4;
      if (chosen(-11) != 47 || !counts(4, 4, 2, 0) || last_inner != 11) return 5;
      throw;
    }
  } catch (const Outer& rethrown) {
    if (&rethrown != original || rethrown.code != 89 || !counts(4, 4, 2, 0)) return 6;
  } catch (...) {
    return 7;
  }
  if (!counts(4, 4, 2, 1)) return 8;

  if (transit(3) != 106 || !counts(5, 5, 2, 1)) return 9;
  try {
    transit(-7);
    return 10;
  } catch (const Inner& propagated) {
    if (propagated.code != 7 || !counts(6, 6, 2, 1)) return 11;
  } catch (...) {
    return 12;
  }
  return counts(6, 6, 3, 1) && last_inner == 7 ? 0 : 13;
}
