// ζ c6 (#88), from the step's review: a constraint on a non-type template parameter is checked
// with the argument as a value of the parameter's type, substituted ([temp.constr.atomic]/3).
// EXIT: 0
template<int N> requires (N > 0) int f() { return 1; }
template<int N> int f() { return 2; }
template<int N> concept Pos = N > 0;
static_assert(Pos<3> && !Pos<-1>);
template<class T, T V> concept Big = V > 100;
static_assert(Big<long, 200> && !Big<int, 5>);
int main() { return f<3>() == 1 && f<0>() == 2 ? 0 : 1; }
