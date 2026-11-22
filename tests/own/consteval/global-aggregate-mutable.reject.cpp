struct Record {
  int ordinary;
  mutable int mutable_field;
};

constexpr Record stored{4, 7};
static_assert(stored.ordinary == 4);
static_assert(stored.mutable_field == 7); //! error
