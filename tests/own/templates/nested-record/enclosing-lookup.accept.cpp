// [class.nest], [temp.local]/7
struct Limit {
  static constexpr unsigned Count = 2;
};

template<unsigned Count>
struct Outer : Limit {
  using Byte = unsigned char;
  struct Record {
    Byte data[Count];
  };
};

// Lookup finds Outer::Byte and the enclosing class's inherited Limit::Count.
static_assert(sizeof(Outer<5>::Record::data) == 2);
