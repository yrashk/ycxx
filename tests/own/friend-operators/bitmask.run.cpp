// #1178: the repro of the issue, and ios_base's shape ([class.friend]/6-7, [over.match.oper],
// [basic.lookup.argdep], [bitmask.types]).
// EXIT: 0
struct S {
  enum E : unsigned { a = 1, b = 2 };
  friend constexpr E operator~(E x) noexcept { return E(~unsigned(x)); }
  friend constexpr E& operator|=(E& x, E y) noexcept { return x = E(unsigned(x) | unsigned(y)); }
};

class Base {
 public:
  enum fmt : unsigned { dec = 1, hex = 2, oct = 4 };
  friend constexpr fmt operator&(fmt x, fmt y) noexcept { return fmt(unsigned(x) & unsigned(y)); }
  friend constexpr fmt operator|(fmt x, fmt y) noexcept { return fmt(unsigned(x) | unsigned(y)); }
  friend constexpr fmt operator^(fmt x, fmt y) noexcept { return fmt(unsigned(x) ^ unsigned(y)); }
  friend constexpr fmt operator~(fmt x) noexcept { return fmt(~unsigned(x)); }
  friend constexpr fmt& operator&=(fmt& x, fmt y) noexcept { return x = x & y; }
  friend constexpr fmt& operator|=(fmt& x, fmt y) noexcept { return x = x | y; }
  friend constexpr fmt& operator^=(fmt& x, fmt y) noexcept { return x = x ^ y; }
  Base() {}
  virtual ~Base() {}
  fmt flags() const noexcept { return flags_; }
  fmt setf(fmt f, fmt mask) noexcept {
    fmt old = flags_;
    flags_ &= ~mask;
    flags_ |= f & mask;
    return old;
  }
  void flip(fmt f) noexcept { flags_ ^= f; }

 private:
  fmt flags_ = dec;
};

constexpr Base::fmt compute() noexcept {
  Base::fmt f = Base::dec;
  (f |= Base::hex) |= Base::oct;
  f &= ~Base::hex;
  f ^= Base::dec;
  return f;
}
static_assert(compute() == Base::oct);
static_assert(noexcept(~Base::dec));
static_assert(__is_same(decltype(~Base::dec), Base::fmt));

int main() {
  S::E e = S::a;
  e |= S::b;
  if (int(~e & 3u)) return 1;
  Base b;
  if (b.setf(Base::hex, Base::fmt(7)) != Base::dec || b.flags() != Base::hex) return 2;
  b.flip(Base::fmt(6));
  if (b.flags() != Base::oct) return 3;
  return compute() - Base::oct;
}
