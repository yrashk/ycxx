// #133: sizeof of an incomplete type ([expr.sizeof]/1).
struct I;
unsigned long n = sizeof(I); //! error
