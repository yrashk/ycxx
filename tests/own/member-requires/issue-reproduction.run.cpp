// #1417 [class.default.ctor], [over.match.viable], [class.mem.general]: the issue's example. A
// constrained defaulted default constructor of a class template is a candidate only when its
// constraint is satisfied.
template <class T> concept DI = requires { T(); };
template <class I> struct B {
  I i;
  B() requires DI<I> = default;
  B(I x) : i(x) {}
};
B<int> b;
int main() { return b.i; }
