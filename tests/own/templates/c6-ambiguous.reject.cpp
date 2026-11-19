// ζ c6 (#88): atomic constraints are identical only when they are the same expression in the source
// ([temp.constr.atomic]/2); two spellings of sizeof(T) > 1 subsume neither way, so the call is
// ambiguous ([temp.constr.order], [over.match.best]).
template<class T> requires (sizeof(T) > 1) int f(T) { return 1; }
template<class T> requires (sizeof(T) > 1) && true int f(T) { return 2; }
int main() { return f(1); } //! error
