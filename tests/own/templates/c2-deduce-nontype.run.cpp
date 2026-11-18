// ζ c2 (#88): function templates deduce a non-type parameter from the argument list of a class
// template specialization (template<int N> int size(const Arr<N>&)), together with a type
// parameter, and of a non-int type (bool, unsigned); the deduced value is usable as a constant
// ([temp.deduct.type], [temp.arg.nontype], [temp.deduct.call]).
// EXIT: 0
template<int N>
struct Arr {
  int data[N];
};

template<class T, int N>
struct Buf {
  T data[N];
};

template<bool B>
struct Flag {};

template<unsigned N>
struct Count {};

template<int N>
int size(const Arr<N>&) { return N; }

template<int N>
int sum(const Arr<N>& a) {
  int s = 0;
  for (int i = 0; i < N; ++i) s += a.data[i];
  return s;
}

template<class T, int N>
T last(Buf<T, N>& b) { return b.data[N - 1]; }

template<int N>
int bound(Arr<N>&) {
  int local[N + 1];
  local[N] = N;
  return local[N];
}

template<bool B>
int flag(Flag<B>&) { return B ? 7 : 8; }

template<unsigned N>
unsigned count(Count<N>*) { return N; }

int main() {
  Arr<3> a{{1, 2, 3}};
  if (size(a) != 3) return 1;
  if (sum(a) != 6) return 2;
  Arr<1> one{{5}};
  if (size(one) != 1) return 3;
  Buf<long, 2> b{{10L, 20L}};
  if (last(b) != 20L) return 4;
  Buf<char, 3> c{{'a', 'b', 'c'}};
  if (last(c) != 'c') return 5;
  if (bound(a) != 3) return 6;
  Flag<true> t{};
  Flag<false> f{};
  if (flag(t) != 7) return 7;
  if (flag(f) != 8) return 8;
  Count<12u> k{};
  if (count(&k) != 12u) return 9;
  return 0;
}
