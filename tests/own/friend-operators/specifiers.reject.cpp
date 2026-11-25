// #1178, [class.friend]/8, [dcl.fct]: specifiers a friend operator function cannot have.
struct A {
    friend virtual int operator~(const A&); //! error
    friend static int operator!(const A&); //! error
    friend int operator+(const A&) const; //! error
    friend explicit int operator*(const A&); //! error
    friend A& operator+=(A&, int) &; //! error
};
