// #257: a failed class assertion leaves a well-formed defaulted comparison.
struct Bad {
  static_assert(false, "class assertion failed"); //! error
  bool operator==(const Bad&) const = default;

  int read() const {
    return missing; //! error
  }
};
