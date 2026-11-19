// ι (#300), from the review:  a defaulted parameter before a trailing pack ([dcl.fct.default]/4, [temp.deduct.call]/1).
// EXIT: 0
template<class... Ts> int f(int a = 1, Ts... ts) { return a + sizeof...(ts); }
int main() { return f() == 1 && f(5, 1, 2) == 7 ? 0 : 1; }
