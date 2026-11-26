template<class To, class From>
constexpr To cast_ref(const From& source) {
  return __builtin_bit_cast(To, source);
}

static_assert(sizeof(int) == 4 && sizeof(unsigned int) == sizeof(int));
static_assert(__is_same(decltype(__builtin_bit_cast(unsigned int, 17)), unsigned int));
static_assert(__is_same(decltype(__builtin_bit_cast(int, 17u)), int));
static_assert(cast_ref<unsigned int>(17) == 17u);
static_assert(cast_ref<unsigned int>(-1) == 0xffffffffu);
static_assert(cast_ref<unsigned int>(-2147483647 - 1) == 0x80000000u);
static_assert(cast_ref<int>(0xffffffffu) == -1);
static_assert(cast_ref<int>(0x80000000u) == -2147483647 - 1);
static_assert(cast_ref<int>(-42) == -42);

constexpr int source_once(int value, int& calls) {
  ++calls;
  return value;
}

constexpr bool once(int value) {
  int calls = 0;
  const unsigned int bits = cast_ref<unsigned int>(source_once(value, calls));
  return calls == 1 && cast_ref<int>(bits) == value;
}
static_assert(once(-7));

unsigned int external_from_signed(const int& source) { return cast_ref<unsigned int>(source); }
int external_from_unsigned(const unsigned int& source) { return cast_ref<int>(source); }

int main(int argc, char**) {
  const int negative = -argc;
  const unsigned int all_ones = 0xffffffffu;
  int calls = 0;
  (void)__builtin_bit_cast(unsigned int, source_once(negative, calls));
  return calls != 1 || !once(negative) ||
         external_from_signed(negative) != static_cast<unsigned int>(negative) ||
         external_from_unsigned(all_ones) != -1;
}
