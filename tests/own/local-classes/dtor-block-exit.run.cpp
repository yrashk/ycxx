// Local classes (#117), spec-only: a user-provided destructor of a local class runs when its
// automatic object goes out of scope at the end of the block ([class.local], [class.dtor],
// [stmt.dcl]).
int main() {
  int log = 0;
  {
    struct Guard {
      int& out;
      ~Guard() { out = out * 10 + 7; }
    };
    Guard g{log};
    if (log != 0) return 1;
  }
  if (log != 7) return 2;
  for (int i = 0; i < 3; ++i) {
    struct Inc {
      int& out;
      ~Inc() { ++out; }
    } inc{log};
  }
  if (log != 10) return 3;
  return 0;
}
