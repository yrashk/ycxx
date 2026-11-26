#if !__has_builtin(__builtin_ctzg)
#error __builtin_ctzg is unavailable
#endif

template <class T>
constexpr bool is_int = false;

template <>
constexpr bool is_int<int> = true;

static_assert(is_int<decltype(__builtin_ctzg(8u))>);
static_assert(__builtin_ctzg(1u) == 0);
static_assert(__builtin_ctzg(21u) == 0);
static_assert(__builtin_ctzg(12u) == 2);
static_assert(__builtin_ctzg(40u) == 3);
static_assert(__builtin_ctzg(1u << 31) == 31);

template <unsigned N>
constexpr int steps() {
  static_assert(is_int<decltype(__builtin_ctzg(static_cast<unsigned>(N)))>);
  constexpr int count = __builtin_ctzg(static_cast<unsigned>(N));
  return count;
}

static_assert(steps<1u>() == 0);
static_assert(steps<40u>() == 3);
static_assert(steps<(1u << 31)>() == 31);

// An uninstantiated template may contain a dependent call with a fallback argument.
template <class T>
constexpr int dependent_fallback(T value) {
  return __builtin_ctzg(value, 32);
}
