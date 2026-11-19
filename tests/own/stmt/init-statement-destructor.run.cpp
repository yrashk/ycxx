// #337 (ι): an object declared in the init-statement of an if, switch or for lives until the end
// of the whole statement, so it is destroyed after whichever branch runs, including the else
// branch ([stmt.if]/4, [stmt.switch]/3, [stmt.for]); with a condition object too, they are
// destroyed in reverse order.
// XFAIL: ycxx needs `explicit operator bool()`, a conversion function (#339)
// EXIT: 0
int trail = 0;
void record(int digit) { trail = trail * 10 + digit; }

struct Guard {
  int id;
  Guard(int i) : id(i) { record(id); }
  ~Guard() { record(id + 5); }
  explicit operator bool() const { return id == 1; }
};

int main() {
  // Construct (1), the then-branch (3), destroy (6).
  if (Guard g(1); g.id == 1) {
    record(3);
  } else {
    record(4);
  }
  if (trail != 136) return 1;

  // The else branch runs before the init-statement object is destroyed.
  trail = 0;
  if (Guard g(2); g.id == 1) {
    record(3);
  } else {
    record(4);
  }
  if (trail != 247) return 2;

  // Init-statement object and condition object: destroyed in reverse order after the statement.
  trail = 0;
  if (Guard a(2); Guard b = Guard(1)) {
    record(9);
  }
  if (trail != 21967) return 3;

  trail = 0;
  switch (Guard g(3); g.id) {
    case 3: record(9);
    default: record(4);
  }
  if (trail != 3948) return 4;

  // Construct (1), then body (9) and increment (0) until trail >= 1000, then destroy (6).
  trail = 0;
  for (Guard g(1); trail < 1000; record(0)) {
    record(9);
  }
  if (trail != 190906) return 5;
  return 0;
}
