// [expr.delete]: the array delete spelling uses empty brackets.
void delete_nonempty(int* p) { delete[1] p; } //! error
