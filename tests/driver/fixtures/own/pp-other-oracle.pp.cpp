// FIXTURE: PASS, Clang's and GCC's -E agree after normalization.
// ORACLE: gcc
#define F(x, y)   x   +   y
int v = F( 1 ,2 );
