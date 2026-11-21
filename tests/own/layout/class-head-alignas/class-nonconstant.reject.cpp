// [dcl.align] Class-head alignment.
extern int requested;
struct alignas(requested) Invalid { char value; }; //! error
