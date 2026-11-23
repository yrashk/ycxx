// #327 __builtin_LINE() requires zero arguments.
int wrong() { return __builtin_LINE(1); } //! error
