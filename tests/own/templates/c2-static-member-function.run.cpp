// ζ c2 (#88): static member functions of a class template, called through the template-id with
// :: and through an object, using the template parameters but no object ([temp.class],
// [class.static.mfct], [expr.prim.id.qual]).
// EXIT: 0
int id(int*) { return 1; }
int id(long*) { return 2; }

template<class T, int N>
struct Info {
  static int size() { return N; }
  static T twice(T x) { return x + x; }
  static int kind() { T t = 0; return id(&t); }
  static int size_plus(int k) { return size() + k; }
  int instance() const { return size() * 10; }
};

int main() {
  if (Info<int, 3>::size() != 3) return 1;
  if (Info<long, 9>::size() != 9) return 2;
  if (Info<int, 0>::twice(21) != 42) return 3;
  if (Info<int, 0>::kind() != 1) return 4;
  if (Info<long, 0>::kind() != 2) return 5;
  if (Info<int, 4>::size_plus(2) != 6) return 6;
  Info<int, 5> i{};
  if (i.size() != 5) return 7;
  if (i.instance() != 50) return 8;
  return 0;
}
