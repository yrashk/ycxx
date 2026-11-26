// Local classes (#117), spec-only: a member function body of a local class is a complete-class
// context, so it can use members (data, functions, types) declared after it in the class
// ([class.mem.general], [class.local]).
int main() {
  struct S {
    int first() { return later() + value + Inner{4}.v; }
    int later() { return value * 2; }
    int value;
    struct Inner { int v; };
  };
  S s{3};
  if (s.first() != 6 + 3 + 4) return 1;
  return 0;
}
