// GCC also locates the implicitly deleted definition. Put the declaration and use on the same marked line.
// [class.copy.assign], [dcl.fct.def.default]
struct Reference { int& value; Reference& operator=(const Reference&) = default; }; void assign(Reference& destination, const Reference& source) { destination = source; //! error }
