struct __attribute__((aligned(16))) Prototype { char c; };
struct Copy { char c; } __attribute__((copy((Prototype*)0))); //! error
