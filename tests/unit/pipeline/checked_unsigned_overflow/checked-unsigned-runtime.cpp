namespace {
volatile unsigned long long add_left = 0xffffffffffffffffULL;
volatile unsigned short add_right = 3;
volatile unsigned long long mul_left = 0x100000001ULL;
volatile unsigned int mul_right = 3;
volatile unsigned int narrow_left = 0xffffffffu;
}

extern "C" {
unsigned long long checked_sum = 17;
unsigned int checked_product[3] = {31, 19, 37};
}

extern "C" int checked_overflow_runtime() {
  volatile unsigned int left_reads = 0;
  volatile unsigned int right_reads = 0;
  volatile unsigned int address_reads = 0;
  const bool sum_overflow = __builtin_add_overflow(
      (left_reads = left_reads + 1u, add_left),
      (right_reads = right_reads + 1u, add_right),
      (address_reads = address_reads + 1u, &checked_sum));
  if (!sum_overflow || checked_sum != 2ULL || left_reads != 1u ||
      right_reads != 1u || address_reads != 1u)
    return 1;

  const bool product_overflow = __builtin_mul_overflow(
      (left_reads = left_reads + 1u, mul_left),
      (right_reads = right_reads + 1u, mul_right),
      (address_reads = address_reads + 1u, &checked_product[1]));
  const volatile unsigned int* const observed = checked_product;
  if (!product_overflow || observed[0] != 31u || observed[1] != 3u ||
      observed[2] != 37u || left_reads != 2u ||
      right_reads != 2u || address_reads != 2u)
    return 2;

  unsigned long long wide = 17;
  if (__builtin_add_overflow(narrow_left, add_right, &wide) ||
      wide != 0x100000002ULL)
    return 3;
  if (__builtin_mul_overflow(narrow_left, mul_right, &wide) ||
      wide != 0x2fffffffdULL)
    return 4;
  return 0;
}
