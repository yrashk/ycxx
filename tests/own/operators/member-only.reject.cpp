// #133: operator= is a member function only ([over.assign]/1).
struct S {};
int operator=(S&, int); //! error
