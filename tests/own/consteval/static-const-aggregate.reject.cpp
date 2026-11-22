struct Record { int value; };

void check() {
  static const int integral = 23;
  static_assert(integral == 23);

  static const Record aggregate{23};
  static_assert(aggregate.value == 23); //! error
}
