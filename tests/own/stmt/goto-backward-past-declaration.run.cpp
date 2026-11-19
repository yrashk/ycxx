// #337 (ι): jumping backward past an initialized automatic variable destroys it, and it is
// initialized again when its declaration is reached ([stmt.dcl]/2); jumping forward past a
// declaration with vacuous initialization is allowed.
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

int forward_past_vacuous(int v) {
  if (v) goto skip;
  int x;
  x = 5;
  return x;
skip:
  x = 7;
  return x;
}

int main() {
  if (backward() != 0) return 1;
  if (constructed != 3) return 2;
  if (destroyed != 3) return 3;
  if (forward_past_vacuous(0) != 5) return 4;
  if (forward_past_vacuous(1) != 7) return 5;
  return 0;
}
