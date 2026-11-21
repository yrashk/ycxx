// [dcl.align] Class-head alignment.
// XFAIL: gcc GCC 16.2 accepts inequivalent alignment on the definition.
struct alignas(16) Announced;
struct alignas(8) Announced { char value; }; //! error
