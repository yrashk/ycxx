// The operand must be an lvalue.
int* invalid = __builtin_addressof(42); //! error
