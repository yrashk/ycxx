// ζ s3 (#88): explicit specializations of a class template (declared, then defined; and defined
// directly, with members of its own), of a function template (with its arguments given and
// deduced from the declared type), and of a variable template; each replaces the template for
// its arguments, in calls, member access and constant evaluation ([temp.expl.spec],
// [temp.deduct.decl]).
// EXIT: 0
template<class T>
struct Traits {
  int size() const { return 1; }
};
template<>
struct Traits<long> {
  int size() const { return 8; }
  int extra() const { return 42; }
};

template<class T> struct Fwd;
template<> struct Fwd<int>;
template<> struct Fwd<int> { int v; };

template<class T> T twice(T x) { return x + x; }
template<> int twice<int>(int x) { return x * 3; }
template<> long twice(long x) { return x - 1; }

template<class T> constexpr int rank = 0;
template<> constexpr int rank<int> = 1;
template<> constexpr int rank<long> = 2;
static_assert(rank<char> == 0 && rank<int> == 1 && rank<long> == 2);

template<class T> constexpr T next(T x) { return x + 1; }
template<> constexpr char next<char>(char x) { return x; }
static_assert(next(1) == 2 && next('a') == 'a');

int main() {
  Traits<int> a;
  Traits<long> b;
  if (a.size() != 1 || b.size() != 8 || b.extra() != 42) return 1;
  if (twice(5) != 15 || twice(5L) != 4L || twice<char>(2) != 4) return 2;
  Fwd<int> f{3};
  if (f.v != 3) return 3;
  return rank<long> == 2 ? 0 : 4;
}
