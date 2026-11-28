// #199, [expr.reinterpret.cast], [expr.const.cast]/7: casting away constness, a prvalue or a
// bit-field to a reference, and conversions the cast does not perform.
struct Bits { int field : 3; };

void invalid(const int* constant, volatile int* changing, char** chars, const int& reference,
             Bits& bits, int value, double real) {
  (void)reinterpret_cast<long*>(constant); //! error
  (void)reinterpret_cast<long*>(changing); //! error
  (void)reinterpret_cast<const char**>(chars); //! error
  (void)reinterpret_cast<long&>(reference); //! error
  (void)reinterpret_cast<long&>(5); //! error
  (void)reinterpret_cast<long&&>(5); //! error
  (void)reinterpret_cast<char&>(bits.field); //! error
  (void)reinterpret_cast<double>(value); //! error
  (void)reinterpret_cast<int>(real); //! error
  (void)reinterpret_cast<short>(constant); //! error
}

template <class T> void dependent(const T* p) {
  (void)reinterpret_cast<long*>(p); //! error
}
void instantiate(const int* p) { dependent(p); }
