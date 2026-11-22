// [over.match.oper]
// XFAIL: clang,gcc Joint ranking of member and nonmember operators is tracked in #311.
struct X { int operator+(int) const &; };
long* operator+(X&&, int);
long* choose() { return X{} + 1; }  //! error
