// GNU __builtin_clz with volatile loads, including implicit width conversion.
// EXIT: 0
extern "C" int witness() {
  volatile unsigned int low = 1u;
  volatile unsigned int middle = 0x10000u;
  volatile unsigned int high = 0x80000000u;
  volatile unsigned long long wide = 0x8000000000000100ULL;

  if (__builtin_clz(low) != 31) return 1;
  if (__builtin_clz(middle) != 15) return 2;
  if (__builtin_clz(high) != 0) return 3;
  if (__builtin_clz(wide) != 23) return 4;
  return 0;
}

int main() { return witness(); }
