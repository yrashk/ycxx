// ζ p4 (#88), from the step's review: a pattern whose only pack is a non-type one inside a type
// (`IC<Is>...`), as a function parameter pack and in a template argument list ([temp.variadic]).
// EXIT: 0
template<int N> struct IC {};
template<class... Ts> struct List { int size() const { return sizeof...(Ts); } };
template<int... Is> int g(const IC<Is>&... xs) { return sizeof...(xs); }
template<int... Is> int f() { return List<IC<Is>...>{}.size(); }
int main() { return f<1, 2>() == 2 ? 0 : 1; }
