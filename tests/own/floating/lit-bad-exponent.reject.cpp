// [lex.fcon], [lex.ppnumber]: an exponent-part needs at least one digit after e and its sign.
double a = 1e; //! error
double b = 1.0e+; //! error
double c = 2.5E-; //! error
double ok = 1e+0;
