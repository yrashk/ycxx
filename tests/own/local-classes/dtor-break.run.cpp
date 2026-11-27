// Local classes (#117), spec-only: break and continue out of a loop body destroy the local-class
// objects declared in that iteration's block ([stmt.break], [stmt.cont], [stmt.jump.general],
// [class.dtor]).
int main() {
  int destroyed = 0;
  int iterations = 0;
  for (int i = 0; i < 10; ++i) {
    struct Count {
      int& out;
      ~Count() { ++out; }
    } c{destroyed};
    ++iterations;
    if (i % 2 == 0) continue;
    if (i == 5) break;
  }
  if (iterations != 6) return 1;
  if (destroyed != 6) return 2;
  int n = 0;
  while (true) {
    struct Mark {
      int& out;
      ~Mark() { out += 100; }
    } m{n};
    break;
  }
  if (n != 100) return 3;
  return 0;
}
