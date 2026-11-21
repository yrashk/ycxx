// [expr.delete]: nullptr alone does not have pointer type.
void delete_null() { delete[] nullptr; } //! error
