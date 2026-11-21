// GCC also locates the implicitly deleted definition. Put the declaration and use on the same marked line.
// [class.copy.assign], [dcl.fct.def.default]
struct Scalar { const int value; Scalar& operator=(const Scalar&) = default; }; void assign(Scalar& destination, const Scalar& source) { destination = source; //! error }
