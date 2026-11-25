struct Halves {
  unsigned short low;
  unsigned short high;
};

struct Bytes {
  Bytes() = delete;
  unsigned char first;
  unsigned char second;
  unsigned char third;
  unsigned char fourth;
};

template<class To, class From>
struct Destination {
  using Type = To;
};

template<class To, class From>
constexpr To recast(const From& source) {
  return __builtin_bit_cast(typename Destination<To, From>::Type, source);
}

template<class From>
constexpr unsigned int as_word(const From& source) {
  return __builtin_bit_cast(unsigned int, source);
}

constexpr unsigned int read_once(unsigned int source, int& reads) {
  ++reads;
  return source;
}

constexpr int check_casts(unsigned int bias) {
  unsigned short low = 0xcdeeu + bias;
  unsigned short high = 0x89abu - bias;
  unsigned int word = (static_cast<unsigned int>(high) << 16) | low;

  // The native targets store these 8-, 16-, and 32-bit fields little-endian.
  Halves halves = recast<Halves>(word);
  if (halves.low != low || halves.high != high) return 1;

  Bytes bytes = recast<Bytes>(word);
  if (bytes.first != (low & 0xffu) || bytes.second != (low >> 8) ||
      bytes.third != (high & 0xffu) || bytes.fourth != (high >> 8)) return 2;

  Bytes from_halves = recast<Bytes>(halves);
  if (from_halves.first != bytes.first || from_halves.second != bytes.second ||
      from_halves.third != bytes.third || from_halves.fourth != bytes.fourth) return 3;
  if (as_word(halves) != word || as_word(bytes) != word) return 4;

  const Halves& independent = __builtin_bit_cast(Halves, halves);
  halves.low = 0;
  if (&independent == &halves || independent.low != low || independent.high != high) return 5;

  int reads = 0;
  Halves once = __builtin_bit_cast(Halves, read_once(word, reads));
  if (reads != 1 || once.low != low || once.high != high) return 6;

  return 0;
}

static_assert(check_casts(1) == 0);

int main(int argc, char**) {
  return check_casts(static_cast<unsigned int>(argc));
}
