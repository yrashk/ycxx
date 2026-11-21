// Substitution checks a written alias argument even when the alias erases it ([temp.alias]).
struct Base {};
template<class T> using Alias = Base;
template<class T> struct Derived : Alias<typename T::Missing> {}; static_assert(sizeof(Derived<int>) > 0); //! error
