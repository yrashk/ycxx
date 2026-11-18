// ζ c2 (#88): in a class template, a template parameter with a default argument must not be
// followed by one without, since a class template's arguments are never deduced to fill the gap
// (function templates allow it) ([temp.param]). One line: Clang reports the error at the
// parameter U, GCC at the class name.
template<class T = int, class U> struct Pair; //! error
