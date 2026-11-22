// #248: the designator must name a member of the specified class ([support.types.layout]).
struct Record {
  int value;
};

static_assert(__builtin_offsetof(Record, value) == 0);
constexpr auto missing_offset = __builtin_offsetof(Record, missing); //! error
