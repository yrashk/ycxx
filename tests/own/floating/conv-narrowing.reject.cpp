// [dcl.init.list]/3.9, [dcl.init.list] (narrowing conversion): list-initialization rejects
// floating to integer (bool included), a floating conversion to lower rank from a non-constant or
// one that overflows, and integer to floating unless a constant that round-trips.
double d = 1.5;
long double ld = 1.5L;
float f = 1.5f;
int i = 3;
long long big = 1;
int a{d}; //! error
int b{1.0}; //! error
bool c{0.0}; //! error
int g{f}; //! error
float h{d}; //! error
double j{ld}; //! error
float k{1e300}; //! error
double l{i}; //! error
float m{16777217}; //! error
double n{9007199254740993LL}; //! error
float o{big}; //! error
char p{65.0}; //! error
struct P { float x; int y; };
P q{d, 1}; //! error
P r{0.5, 2.0}; //! error
float ret(double x) { return {x}; } //! error
