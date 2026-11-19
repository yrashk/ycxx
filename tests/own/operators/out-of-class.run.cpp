// #133, from the step's review: member operators defined outside their class and a namespace's
// operator defined outside it ([class.mfct], [namespace.memdef]).
// EXIT: 0
struct S { int v; int operator+(int) const; };
int S::operator+(int k) const { return v + k; }
namespace N {
struct T { int w; };
int operator+(const T&, int);
}
int N::operator+(const T& t, int k) { return t.w * k; }
int main() {
  S s{2};
  N::T t{3};
  return (s + 5) - 7 + (t + 2) - 6;
}
