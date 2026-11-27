// #356: an unqualified call with a dependent argument in a member's default argument is looked
// up again at the point of instantiation: argument-dependent lookup finds a function declared
// after the class template but before the use ([temp.dep.candidate], [basic.lookup.argdep],
// [temp.point], [temp.inst]).
// EXIT: 0
template<class T>
struct Reader {
  int get(int x = read(T{})) { return x; }
  Reader(int x = read(T{}) * 2) : v(x) {}
  int v;
};

namespace lib {
struct Tag {};
}

namespace lib {
int read(Tag) { return 21; }
}

namespace other {
struct Mark {};
int read(Mark) { return 4; }
}

int main() {
  Reader<lib::Tag> r;
  if (r.v != 42) return 1;
  if (r.get() != 21) return 2;
  if (r.get(3) != 3) return 3;
  Reader<other::Mark> m;
  if (m.get() != 4 || m.v != 8) return 4;
  return 0;
}
