// ι (#300), from the review:  an operator function other than operator() and operator[] has no default arguments
// ([over.oper.general]).
struct S { int v; };
bool operator==(S a, S b = S{1}) { return a.v == b.v; } //! error
int main() { return 0; }
