// #1400 (review): a mem-initializer-id that is a constructor template's type parameter names a
// concrete base only in each specialization ([temp.dep.type], [class.base.init]): the definition
// must not default-initialize the class's bases, which here have no default constructor. A pack
// of them expands to one mem-initializer per base ([temp.variadic]).
// EXIT: 0
struct A { int a; A(int x) : a(x) {} };
struct B { int b; B(int x) : b(x) {} };
struct One : A { template<class T> One(T t) : T(t) {} };
struct Two : A, B { template<class... Ts> Two(Ts... ts) : Ts(ts)... {} };
int main() {
  One one(A(4));
  if (one.a != 4) return 1;
  Two two(A(1), B(2));
  if (two.a != 1 || two.b != 2) return 2;
  Two swapped(B(5), A(6));
  if (swapped.a != 6 || swapped.b != 5) return 3;
  return 0;
}
