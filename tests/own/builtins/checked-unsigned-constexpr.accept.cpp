constexpr bool parser_steps() {
  unsigned char value = 5;
  if (__builtin_mul_overflow(value, 7u, &value) || value != 35)
    return false;
  if (__builtin_add_overflow(value, 7u, &value) || value != 42)
    return false;
  value = 250;
  if (!__builtin_mul_overflow(value, 2u, &value) || value != 244)
    return false;
  return __builtin_add_overflow(value, 12u, &value) && value == 0;
}

constexpr bool add_widths() {
  unsigned short small = 65535;
  unsigned long wide = 17;
  unsigned int narrow = 19;
  unsigned short shortest = 65534;
  if (__builtin_add_overflow(shortest, 1u, &shortest) || shortest != 65535u)
    return false;
  if (!__builtin_add_overflow(shortest, 1u, &shortest) || shortest != 0u)
    return false;
  if (__builtin_add_overflow(0xffffffffu, small, &wide) ||
      wide != 0x10000fffeUL)
    return false;
  if (!__builtin_add_overflow(0x100000000ULL, 1u, &narrow) ||
      narrow != 1u)
    return false;
  if (__builtin_add_overflow(0xfffffffffffffffeULL, 1u, &wide) ||
      wide != 0xffffffffffffffffUL)
    return false;
  return __builtin_add_overflow(wide, 1u, &wide) && wide == 0UL;
}

constexpr bool mul_widths() {
  unsigned short two = 2;
  unsigned long long wide = 17;
  unsigned int narrow = 19;
  if (__builtin_mul_overflow(0xffffffffu, two, &wide) ||
      wide != 0x1fffffffeULL)
    return false;
  if (!__builtin_mul_overflow(0xffffffffu, two, &narrow) ||
      narrow != 0xfffffffeu)
    return false;
  if (!__builtin_mul_overflow(0x8000000000000001ULL, two, &wide) ||
      wide != 2ULL)
    return false;
  if (__builtin_mul_overflow(0x5555555555555555ULL, 3u, &wide) ||
      wide != 0xffffffffffffffffULL)
    return false;
  return !__builtin_mul_overflow(wide, 0u, &wide) && wide == 0ULL;
}

static_assert(parser_steps());
static_assert(add_widths());
static_assert(mul_widths());
