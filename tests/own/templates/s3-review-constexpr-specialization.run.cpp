// ζ s3 (#88), from the step's review: an explicit specialization is constexpr as it is
// declared, not as its template is ([temp.expl.spec], [dcl.constexpr]).
// EXIT: 0
template<class T> int f(T) { return 1; }
template<> constexpr int f<int>(int) { return 7; }
static_assert(f(1) == 7);
int main() { return f(1) == 7 && f('a') == 1 ? 0 : 1; }
