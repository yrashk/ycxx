// ζ p4 (#88), from the step's review: an out-of-class member definition may name the class's
// function parameter pack anew; every element takes the definition's name ([temp.variadic]).
// EXIT: 6
template<class... Ts> struct S {
  int f(Ts... xs);
};
template<class... Ts> int S<Ts...>::f(Ts... ys) { return (0 + ... + ys); }
int main() { S<int, int, int> s; return s.f(1, 2, 3); }
