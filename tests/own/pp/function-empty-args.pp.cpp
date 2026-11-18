// [cpp.replace.general]/12, [cpp.subst]: an argument may consist of no preprocessing tokens;
// a macro with one parameter invoked with `()` gets one empty argument, and empty arguments
// substitute nothing.
#define one(x) [x]
#define two(a, b) [a|b]
#define three(a, b, c) [a|b|c]
#define EMPTY
one();
one( );
two(,);
two(a,);
two(,b);
three(,,);
three(a,,c);
one(EMPTY);
two(EMPTY, EMPTY);
