// #1172: __int128 combines only with `signed` or `unsigned` ([dcl.type.general]).
// FLAGS: -Wno-pedantic
long __int128 a; //! error
__int128 int b; //! error
short __int128 c; //! error
unsigned signed __int128 d; //! error
__int128 __int128 e; //! error
char __int128 f; //! error
__int128 double g; //! error
