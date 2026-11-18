// ζ c2 (#88): non-type template parameters of several integral types (bool, char, unsigned,
// long), alone and mixed with a type parameter in either order, read inside member functions
// ([temp.param], [temp.arg.nontype], [temp.class]).
// EXIT: 0
template<bool B>
struct Flag {
  int value() const { return B ? 1 : 2; }
};

template<char C>
struct Letter {
  bool is(char c) const { return c == C; }
};

template<unsigned N, class T>
struct Repeat {
  T unit;
  T total() const {
    T s = 0;
    for (unsigned i = 0; i < N; ++i) s += unit;
    return s;
  }
};

template<class T, long K>
struct Offset {
  T base;
  T at() const { return base + K; }
};

int main() {
  Flag<true> t{};
  Flag<false> f{};
  if (t.value() != 1) return 1;
  if (f.value() != 2) return 2;
  Letter<'z'> z{};
  if (!z.is('z')) return 3;
  if (z.is('a')) return 4;
  Repeat<3u, int> r{4};
  if (r.total() != 12) return 5;
  Repeat<0u, long> e{9L};
  if (e.total() != 0L) return 6;
  Offset<int, -4L> o{10};
  if (o.at() != 6) return 7;
  Offset<char, 1L> n{'a'};
  if (n.at() != 'b') return 8;
  return 0;
}
