// A non-template member's trailing requires-clause is part of its mangled name, so a function-local
// static is shared with objects other compilers produce (Itanium C++ ABI §5.1.2, `Q`).
template<class T> concept Four = sizeof(T) == 4;
template<class T> struct S {
  int next() requires Four<T> { static int counter = 0; return ++counter; }
  S() requires Four<T> {}
};
