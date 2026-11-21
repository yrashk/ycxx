// XFAIL: gcc GCC accepts delete[] on void* with a warning.
// [expr.delete]: a pointer to void does not point to an object.
void delete_void(void* p) { delete[] p; } //! error
