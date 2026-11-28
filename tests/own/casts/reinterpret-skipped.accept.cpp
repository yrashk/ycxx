// #199, [expr.const], [stmt.if]: a reinterpret_cast that constant evaluation does not reach,
// in the shape of libycxx's char_traits::length and __load_le, leaves the evaluation constant.
template <class T> constexpr unsigned long length(const T* s) {
  if constexpr (sizeof(T) == 1) {
    if !consteval {
      return __builtin_strlen(reinterpret_cast<const char*>(s));
    }
  }
  unsigned long n = 0;
  while (s[n] != T()) ++n;
  return n;
}
static_assert(length("abcd") == 4);
static_assert(length(u8"ab") == 2);

constexpr const unsigned char* view(bool reach, const char* p) {
  return reach ? reinterpret_cast<const unsigned char*>(p) : nullptr;
}
static_assert(view(false, "x") == nullptr);

unsigned long runtime = length("xyz");
