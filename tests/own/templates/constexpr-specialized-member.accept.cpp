constexpr int select(long) { return 5; }
template<class T> struct Only {
  static constexpr int value = sizeof(T);
  static constexpr int qualified() { return Only::value + select(0); }
  static constexpr int unused() { return T::missing; }
};
constexpr int select(int) { return 9; }

static_assert(Only<char>::qualified() == 6);
static_assert(Only<long long>::qualified() == 13);
static_assert(Only<char>::qualified() == 6);
