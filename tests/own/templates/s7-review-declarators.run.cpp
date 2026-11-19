// ζ s7 (#88), from the step's review: an earlier declarator of the same member declaration is value-dependent too
// ([temp.dep.constexpr]/2.3).
// EXIT: 0
template<class T> struct S { static constexpr int a = sizeof(T), b = a * 2; };
int main() { return 0; }
