// #1424, [expr.const.cast]: reject different shapes, invalid categories, and a bit-field reference.
struct Bits { int field : 3; };
struct Base {};
struct Derived : Base {};

using FunctionPointer = void (*)();
void function();

void invalid(const int* pointer, const Base* base, const int& reference,
             const int (*array)[2], Bits& bits) {
  (void)const_cast<double*>(pointer); //! error
  (void)const_cast<void*>(pointer); //! error
  (void)const_cast<const Derived*>(base); //! error
  (void)const_cast<FunctionPointer>(&function); //! error
  (void)const_cast<int>(0); //! error
  (void)const_cast<int&&>(0); //! error
  (void)const_cast<int&>(static_cast<const int&&>(reference)); //! error
  (void)const_cast<const int (*)[]>(array); //! error
  (void)const_cast<int*>(nullptr); //! error
  (void)const_cast<int&>(bits.field); //! error
  (void)const_cast<int&&>(bits.field); //! error
}
