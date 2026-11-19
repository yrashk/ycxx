// #133: a non-member operator function has a parameter of class or enumeration type ([over.oper.general]/7).
int operator+(int a, int b); //! error
