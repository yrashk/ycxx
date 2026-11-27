// Local classes (#117), spec-only: leaving a function by an early return destroys the automatic
// local-class objects already constructed, and not the ones not yet reached ([stmt.return],
// [stmt.jump.general], [class.dtor]).
int f(int& log, bool early) {
  struct Tag {
    int& out;
    int id;
    ~Tag() { out = out * 10 + id; }
  };
  Tag a{log, 1};
  if (early) return 5;
  Tag b{log, 2};
  return 6;
}

int main() {
  int log = 0;
  if (f(log, true) != 5) return 1;
  if (log != 1) return 2;
  log = 0;
  if (f(log, false) != 6) return 3;
  if (log != 21) return 4;
  return 0;
}
