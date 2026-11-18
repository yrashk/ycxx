// #133: sizeof of a function ([expr.sizeof]/1).
int f();
unsigned long n = sizeof(f); //! error
