// [temp.alias]/3: substitution still checks an argument erased by the aliased type.
template<bool> using Discard = long;
template<class T> using Erase = Discard<__array_rank(T)>; Erase<char[2][2]> bad; //! error
