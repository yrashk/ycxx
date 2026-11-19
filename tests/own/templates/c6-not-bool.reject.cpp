// ζ c6 (#88): an atomic constraint's substituted expression must have type bool
// ([temp.constr.atomic]/3); no conversion applies. (GCC also reports the static_assert, Clang
// only the atom; one line keeps them in agreement.)
template<class T> concept One = sizeof(T); static_assert(One<int>); //! error
int main() { return 0; }
