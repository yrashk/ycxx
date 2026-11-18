// ζ f1 (#88): non-type template parameters of integral types (int, unsigned, long, bool, char),
// alone and after or before a type parameter, given as constant expressions and used inside the
// body as an operand, a condition and an array bound ([temp.param], [temp.arg.nontype],
// [temp.arg.explicit]).
// EXIT: 0
template<int N>
int times(int x) { return x * N; }

template<unsigned N>
int fill_and_sum(int v) {
  int buf[N];
  for (unsigned i = 0; i < N; ++i) buf[i] = v;
  int s = 0;
  for (unsigned i = 0; i < N; ++i) s += buf[i];
  return s;
}

template<bool Negate>
int maybe_negate(int x) {
  if (Negate) return -x;
  return x;
}

template<char C>
bool is(char c) { return c == C; }

template<class T, long K>
T add_k(T x) { return x + K; }

template<int N, class T>
T scaled(T x) { return x * N; }

constexpr int four = 4;

int main() {
  if (times<3>(7) != 21) return 1;
  if (times<-2>(4) != -8) return 2;
  if (times<2 + 3>(1) != 5) return 3;
  if (times<four>(2) != 8) return 4;
  if (fill_and_sum<5>(3) != 15) return 5;
  if (fill_and_sum<1u>(9) != 9) return 6;
  if (maybe_negate<true>(6) != -6) return 7;
  if (maybe_negate<false>(6) != 6) return 8;
  if (!is<'q'>('q')) return 9;
  if (is<'q'>('r')) return 10;
  if (add_k<int, 5>(3) != 8) return 11;
  if (add_k<long, -3L>(10L) != 7L) return 12;
  if (add_k<char, 1>('a') != 'b') return 13;
  if (scaled<3>(5L) != 15L) return 14;
  if (scaled<four * 2>('\1') != 8) return 15;
  return 0;
}
