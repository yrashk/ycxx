// [expr.cond] A conditional with one throw operand can designate a writable bit-field.
struct Bits { unsigned value : 3; };

void assign(bool choose, Bits& bits) {
  (choose ? (throw 0) : bits.value) = 2;
}
