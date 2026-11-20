// A compatible cast can add volatile. A reference binding preserves it ([dcl.init.ref], #199).
void probe() {
  int val = 1;
  const int& ref = static_cast<volatile int&&>(val);  //! error
}
