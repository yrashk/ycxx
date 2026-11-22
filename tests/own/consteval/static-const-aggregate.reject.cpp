struct Record { int value; };

constexpr int read_static(bool reach_static) {
  if (reach_static) {
    static const Record aggregate{23};
    return aggregate.value;
  }
  return 0;
}

template<int Value> struct Result {};
template<bool Reach>
concept Readable = requires {
  typename Result<read_static(Reach)>;
};

static_assert(!Readable<true>);

void check() {
  static const int integral = 23;
  static_assert(integral == 23);

  static const Record aggregate{23};
  static_assert(aggregate.value == 23); //! error
}
