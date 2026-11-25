// EXIT: 0

template<bool Enabled>
struct Counter {
  static long leaf(long value) {
    static_assert(Enabled, "leaf must be enabled");
    return value + 1L;
  }

  static long helper(long value) { return leaf(value); }
  static auto wrapper(long value) { return helper(value); }
};

static_assert(sizeof(Counter<true>::wrapper(0L)) == sizeof(long));

int main() { return 0; }
