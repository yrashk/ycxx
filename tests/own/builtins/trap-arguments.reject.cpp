void one_argument() { __builtin_trap(0); } //! error
void two_arguments() { __builtin_trap(0, 1); } //! error
void zero_arguments() { __builtin_trap(); }
