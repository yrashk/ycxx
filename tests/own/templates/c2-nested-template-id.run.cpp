// ζ c2 (#88): a specialization as the argument of another: Box<Box<int>>, whose >> closes both
// argument lists ([temp.names]/4), nested three deep, and mixed with a non-type parameter; the
// inner object is reached through members and references, never copied ([temp.names],
// [temp.arg.type], [temp.class]).
// EXIT: 0
template<class T>
struct Box {
  T v;
  T& ref() { return v; }
  const T& cref() const { return v; }
};

template<class T, int N>
struct Many {
  T items[N];
  T& at(int i) { return items[i]; }
};

int main() {
  Box<Box<int>> bb{{5}};
  if (bb.v.v != 5) return 1;
  if (bb.ref().ref() != 5) return 2;
  bb.ref().ref() = 6;
  if (bb.cref().cref() != 6) return 3;
  Box<Box<Box<char>>> bbb{{{'q'}}};
  if (bbb.ref().ref().ref() != 'q') return 4;
  Many<Box<int>, 3> m{{{1}, {2}, {3}}};
  if (m.at(2).v != 3) return 5;
  Box<Many<int, 2>> bm{{{7, 8}}};
  if (bm.ref().at(1) != 8) return 6;
  Box<Box<int>>* p = &bb;
  if (p->v.v != 6) return 7;
  return 0;
}
