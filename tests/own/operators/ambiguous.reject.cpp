// #133, from the step's review: ambiguous operator functions make the expression ill-formed
// ([over.match.best]).
enum E { a = 1, b = 2 };
int operator|(E, int);
int operator|(int, E);
int f() { return a | b; } //! error
