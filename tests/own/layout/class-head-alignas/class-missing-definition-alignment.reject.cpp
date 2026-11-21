// [dcl.align] Class-head alignment.
// XFAIL: gcc GCC 16.2 accepts a definition with the alignment omitted.
struct alignas(16) Announced;
struct Announced { char value; }; //! error
