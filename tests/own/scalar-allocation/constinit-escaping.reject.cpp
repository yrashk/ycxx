// [expr.const], [dcl.constinit]: this allocation would escape constant evaluation.
constinit int* p = new int(41); //! error
