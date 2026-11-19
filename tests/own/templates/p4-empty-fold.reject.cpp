// ζ p4 (#88): a unary fold over an empty pack has a value only for `&&`, `||` and `,`
// ([temp.variadic]/12).
template<class... Ts> int sum(Ts... xs) { return (... + xs); } //! error
int main() { return sum(); }
