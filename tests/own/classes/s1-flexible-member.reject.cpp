// δ s1 (#86), from the step's review: a non-static data member cannot be an array of unknown
// bound ([class.mem.general]: its type is complete).
struct S { int n; int a[]; }; //! error
int main() { return 0; }
