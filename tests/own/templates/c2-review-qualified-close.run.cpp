// ζ c2 (#88), from the step's review: `>>` closing a qualified template-id's list and its
// enclosing one, and before `::` ([temp.names]/4).
// EXIT: 0
namespace N { template<class T> struct Box { T v; }; }
template<class T> struct B { T v; static int s() { return 3; } };
int main() {
  N::Box<N::Box<int>> bb{{3}};
  if (bb.v.v != 3) return 1;
  if (B<B<int>>::s() != 3) return 2;
  return 0;
}
