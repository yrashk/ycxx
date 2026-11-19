// ζ p4 (#88), from the step's review: a C ellipsis after a function parameter pack, and after a
// named parameter without a comma ([dcl.fct]/3).
// EXIT: 0
template<class... Ts> int v(Ts......) { return sizeof...(Ts); }
int k(int x...) { return x; }
int main() { return v(1, 2) == 2 && k(3, 4) == 3 ? 0 : 1; }
