// [dcl.stc], [basic.link], [basic.start.main], [basic.scope.param], [lex.icon]: storage classes,
// redeclarations with another type, main as a variable, duplicate parameters, and an integer
// literal no type can hold.
static extern int both; //! error
extern int x;
extern const int x; //! error
int v = 1;
extern int v, v = 2; //! error
int g(int a, int a) { return a; } //! error
long too_big = 9223372036854775808; //! error
int main; //! error
