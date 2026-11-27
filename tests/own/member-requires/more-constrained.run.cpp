// #1417 [over.match.best.general], [temp.constr.order]: of two satisfied non-template member
// functions with the same parameter types, the one whose constraints subsume the other's is
// better; an unconstrained one is the least constrained of all.
template<class T> inline constexpr bool integral = false;
template<> inline constexpr bool integral<int> = true;
template<> inline constexpr bool integral<unsigned> = true;
template<class T> concept Integral = integral<T>;
template<class T> concept SignedIntegral = Integral<T> && (T(-1) < T(0));

template<class T>
struct Classify {
  int which() { return 1; }
  int which() requires Integral<T> { return 2; }
  int which() requires SignedIntegral<T> { return 3; }
};

int main() {
  Classify<double> d;
  Classify<unsigned> u;
  Classify<int> i;
  if (d.which() != 1) return 1;
  if (u.which() != 2) return 2;
  if (i.which() != 3) return 3;
  return 0;
}
