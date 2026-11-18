// ζ f1 (#88): explicitly specified template arguments: all of them, or a prefix with the rest
// deduced; an explicit argument fixes a parameter type, so the call converts arguments that
// deduction alone would reject as conflicting; an empty list `f<>` still deduces but considers
// only templates ([temp.arg.explicit], [temp.deduct.call], [over.match.best]).
// EXIT: 0
template<class R, class T>
R convert(T x) { return x; }

template<class T>
T larger(T a, T b) {
  if (a < b) return b;
  return a;
}

template<int N, class T>
T scaled(T x) { return x * N; }

int g(int) { return 1; }
template<class T>
int g(T) { return 2; }

int main() {
  if (convert<char>(321) != 'A') return 1;
  if (convert<bool>(7) != true) return 2;
  if (convert<long, char>('b') != 98L) return 3;
  if (larger<long>(3, 5L) != 5L) return 4;
  if (larger<int>('a', 1) != 97) return 5;
  if (larger<char>(66, 'A') != 'B') return 6;
  if (scaled<3>(7) != 21) return 7;
  if (scaled<2, long>(5) != 10L) return 8;
  if (scaled<4, char>(true) != 4) return 9;
  if (g(1) != 1) return 10;
  if (g<>(1) != 2) return 11;
  if (g<int>(1) != 2) return 12;
  if (g<long>('c') != 2) return 13;
  return 0;
}
