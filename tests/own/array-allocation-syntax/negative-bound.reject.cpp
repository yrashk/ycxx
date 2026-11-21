// [expr.new]: check a constant bound before its conversion to size_t.
int* negative() { return new int[-1]; } //! error
