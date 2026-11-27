// Local classes (#117), spec-only: two automatic objects of a local class are destroyed in the
// reverse order of their construction at block exit ([class.dtor], [stmt.jump.general]).
int main() {
  int log = 0;
  {
    struct Tag {
      int& out;
      int id;
      ~Tag() { out = out * 10 + id; }
    };
    Tag first{log, 1};
    Tag second{log, 2};
    Tag third{log, 3};
  }
  if (log != 321) return 1;
  return 0;
}
