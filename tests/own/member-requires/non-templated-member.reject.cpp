// #1417 [dcl.decl.general]: a trailing requires-clause may appear only in the declaration of a
// templated function; a member function of a non-template class that is not itself a template
// is not templated, so its requires-clause is ill-formed.
struct Plain {
  int f() requires true; //! error
};
