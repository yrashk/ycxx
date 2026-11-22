struct B { virtual int identify(); };

void* discard_const(const B* source) { return dynamic_cast<void*>(source); } //! error
const void* discard_volatile(const volatile B* source) { return dynamic_cast<const void*>(source); } //! error
