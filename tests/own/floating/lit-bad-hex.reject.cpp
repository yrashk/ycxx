// [lex.fcon], [lex.ppnumber]: a hexadecimal floating literal needs a binary-exponent-part with
// digits; a period without one, or p without digits, is ill-formed.
double a = 0x1.8; //! error
double b = 0x1p; //! error
double c = 0x1.8p+; //! error
double ok = 0x1.8p1;
