// #194: sizeof of an incomplete class is ill-formed ([expr.sizeof]/1), also inside its own
// member-specification.
struct I;
unsigned long a = sizeof(I);  //! error
struct T { int x[sizeof(T)]; };  //! error
int main() { return 0; }
