// δ s1 (#86), from the step's review: pointer arithmetic needs a completely-defined object type
// ([expr.add]).
struct I;
int f(I* p) { return p + 1 == p; } //! error
int main() { return 0; }
