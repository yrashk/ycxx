// [expr.cond] The surviving bit-field remains subject to the address-of restriction.
struct Bits { unsigned value : 3; };

void select(bool choose, Bits& bits) {
  &(choose ? bits.value : throw 0); //! error
}
