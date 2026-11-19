// [lex.fcon], [lex.icon], [lex.ext]: f is a floating-point-suffix but not an integer-suffix, and a
// floating literal takes at most one suffix; any other suffix would name a user-defined literal,
// and none is declared.
float a = 1f; //! error
float b = 1.0ff; //! error
double c = 1.0fl; //! error
double d = 1.0lf; //! error
float ok = 1.f;
