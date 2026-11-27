constexpr bool signed_long_control() {
  long result = 17;
  if (__builtin_add_overflow(-17L, 9L, &result) || result != -8L)
    return false;
  return !__builtin_mul_overflow(-7L, 6L, &result) && result == -42L;
}

static_assert(signed_long_control());
