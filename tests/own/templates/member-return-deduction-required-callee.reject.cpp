template<bool Enabled>
struct Counter {
  static long leaf(long value) {
    static_assert(Enabled, "leaf must be enabled"); //! error
    return value + 1L;
  }

  static long helper(long value) { return leaf(value); }
  static auto wrapper(long value) { return helper(value); }
};

static_assert(sizeof(Counter<false>::wrapper(0L)) == sizeof(long));
