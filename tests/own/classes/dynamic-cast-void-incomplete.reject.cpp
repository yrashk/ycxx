struct B;

void* cast(B* source) { return dynamic_cast<void*>(source); } //! error

struct B { virtual int identify(); };
