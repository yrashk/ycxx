// #133: sizeof of an array of unknown bound, an incomplete type ([expr.sizeof]/1).
extern int a[];
unsigned long n = sizeof(a); //! error
