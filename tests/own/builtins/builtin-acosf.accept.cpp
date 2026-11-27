// The float C-library spelling and its ordinary argument conversions.
#if !__has_builtin(__builtin_acosf)
#error __builtin_acosf is unavailable
#endif
static_assert(__has_builtin(__builtin_acosf));

template<class T> struct float_result {
  static constexpr bool value = false;
};
template<> struct float_result<float> {
  static constexpr bool value = true;
};

struct ToFloat { operator float() const; };

static_assert(float_result<decltype(__builtin_acosf(0.5f))>::value);
static_assert(float_result<decltype(__builtin_acosf(0.5))>::value);
static_assert(float_result<decltype(__builtin_acosf(1))>::value);
static_assert(float_result<decltype(__builtin_acosf(ToFloat{}))>::value);
