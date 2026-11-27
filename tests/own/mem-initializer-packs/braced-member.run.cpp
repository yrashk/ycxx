// #1400: pack expansions in a member's braced mem-initializer ([class.base.init],
// [temp.variadic], [dcl.init.list]): aggregate initialization of an aggregate member, a
// constructor call for a class member, and an array member with an element after the expansion.
// EXIT: 0
struct Agg { int a, b, c; };
struct WithCtor { int sum; WithCtor(int x, int y) : sum(x * 10 + y) {} };

template<class... A> struct Aggregates {
  Agg value;
  Aggregates(A... args) : value{args...} {}
};
template<class... A> struct Constructed {
  WithCtor value;
  Constructed(A... args) : value{args...} {}
};
template<class... A> struct Array {
  int n[4];
  Array(A... args) : n{args..., 9} {}
};

int main() {
  Aggregates<int, int, int> full(1, 2, 3);
  if (full.value.a != 1 || full.value.b != 2 || full.value.c != 3) return 1;
  Aggregates<int> partial(4);
  if (partial.value.a != 4 || partial.value.b != 0 || partial.value.c != 0) return 2;

  Constructed<int, int> constructed(3, 4);
  if (constructed.value.sum != 34) return 3;

  Array<int, int> array(5, 6);
  if (array.n[0] != 5 || array.n[1] != 6 || array.n[2] != 9 || array.n[3] != 0) return 4;
  return 0;
}
