// ζ p4 (#88), from the Fable check of the pack amendment: `sizeof...` in a function template's
// signature, mangled `sZ T_` as Itanium says ([expr.sizeof], [temp.variadic]).
// EXIT: 0
template<int N> struct IC {};
template<class... Ts> IC<sizeof...(Ts)>* k(Ts...) { return nullptr; }
int main() { return k(1, (char)2) == nullptr ? 0 : 1; }
