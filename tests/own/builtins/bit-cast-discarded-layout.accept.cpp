struct alignas(16) ExtendedLayout {
  unsigned long long significand;
  unsigned short sign_exponent;
};

struct Halves {
  unsigned short low;
  unsigned short high;
};

template<class Unsigned, class From>
constexpr Unsigned retained(const From& source) {
  if constexpr (sizeof(From) == sizeof(ExtendedLayout)) {
    return __builtin_bit_cast(ExtendedLayout, source).sign_exponent;
  } else {
    using Word = Unsigned;
    return __builtin_bit_cast(Word, source);
  }
}

static_assert(retained<unsigned int>(Halves{0xcdefu, 0x89abu}) == 0x89abcdefu);
static_assert(retained<unsigned short>(static_cast<unsigned short>(0x9147u)) == 0x9147u);
