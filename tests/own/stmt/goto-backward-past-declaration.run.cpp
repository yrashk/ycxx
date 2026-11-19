// #337 (ι): jumping backward past an initialized automatic variable destroys it, and it is
// initialized again when its declaration is reached ([stmt.dcl]/2). The forward jump past a vacuous
// initialization is in goto-forward-past-vacuous.run.cpp.
// EXIT: 0
int constructed = 0;
int destroyed = 0;

struct Tracker {
  Tracker() { ++constructed; }
  ~Tracker() { ++destroyed; }
};

int backward() {
  int round = 0;
again:
  ++round;
  Tracker t;
  if (constructed != destroyed + 1) return 1;
  if (round < 3) goto again;
  return 0;
}

int main() {
  if (backward() != 0) return 1;
  if (constructed != 3) return 2;
  if (destroyed != 3) return 3;
  return 0;
}
