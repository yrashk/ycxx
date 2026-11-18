// #133, from the step's review: a class template's union is a union in every specialization
// ([class.union], [temp.inst]); a union's first member takes its default member initializer.
// EXIT: 0
template <class T>
union U {
  T a;
  char c;
};
union First { int i = 4; char c; };
First g;
int main() {
  U<int> u{};
  u.c = 3;
  First f;
  return u.a - 3 + f.i + g.i - 8;
}
