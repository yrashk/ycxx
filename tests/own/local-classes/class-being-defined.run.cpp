// #117: the shape of libycxx's tuple_cat plan: a local class in a lambda that initializes a static
// constexpr data member of a class template names a nested class and a static member declared
// before it in the class being defined ([class.local], [basic.scope.class], [expr.prim.lambda]).
// XFAIL: gcc GCC 16 finds none of Plan's members from Pick (#117, docs/compatibility.toml)
template <int N> struct Plan {
  static constexpr int count = N;
  struct Entry {
    int outer;
  };
  static constexpr auto first = [] {
    struct Pick {
      int index = count - 1;
      int doubled() const { return index * 2; }
    } p{};
    return p;
  }();
  static int last() {
    Entry e{first.index};
    return e.outer;
  }
};

int main() {
  if (Plan<4>::first.index != 3 || Plan<4>::last() != 3) return 1;
  if (Plan<7>::first.doubled() != 12) return 2;
  return 0;
}
