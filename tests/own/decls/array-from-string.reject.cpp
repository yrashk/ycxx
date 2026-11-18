// #133: only a character array is initialized by a string literal ([dcl.init.string]/1).
int a[] = "x"; //! error
