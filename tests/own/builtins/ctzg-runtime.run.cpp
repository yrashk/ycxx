int main() {
  volatile unsigned int one = 1u;
  volatile unsigned int odd = 21u;
  volatile unsigned int multiple_bits = 12u;
  volatile unsigned int high_bit = 1u << 31;

  if (__builtin_ctzg(one) != 0 || __builtin_ctzg(odd) != 0)
    return 1;
  if (__builtin_ctzg(multiple_bits) != 2 || __builtin_ctzg(high_bit) != 31)
    return 2;

  unsigned int value = multiple_bits;
  if (__builtin_ctzg(value++) != 2 || value != 13u)
    return 3;

  return 0;
}
