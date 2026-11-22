// #248: the builtin diagnoses a bit-field designator, whose address cannot be taken
// ([support.types.layout], [expr.unary.op]).
struct Flags {
  int value;
  unsigned int bits : 2;
};

static_assert(__builtin_offsetof(Flags, value) == 0);
constexpr auto bits_offset = __builtin_offsetof(Flags, bits); //! error
