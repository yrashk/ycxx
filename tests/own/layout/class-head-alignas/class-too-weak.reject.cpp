// [dcl.align] Class-head alignment.
// XFAIL: gcc GCC 16.2 accepts a request below the natural alignment.
struct alignas(8) Member { char value; };
struct alignas(1) TooWeak { Member value; }; //! error
