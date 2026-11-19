// #133, from the step's review: a member operator takes the operator's arity less the object
// ([over.binary]/1).
struct S { int operator+(int, int); }; //! error
