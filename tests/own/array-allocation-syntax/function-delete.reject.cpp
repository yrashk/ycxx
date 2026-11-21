// [expr.delete]: a direct function pointer does not point to an object.
void delete_function(void (*p)()) { delete[] p; } //! error
