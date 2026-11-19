// #133, from the step's review: an operator function other than () and [] is not static
// ([over.oper.general]/7).
struct S { static int operator+(S, S); }; //! error
