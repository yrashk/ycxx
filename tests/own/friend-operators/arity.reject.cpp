// #1178, [over.oper.general], [over.unary], [over.binary], [over.inc]: a nonmember operator has
// exactly the number of parameters its operator's uses need, with no default arguments.
struct A {
    friend int operator~(); //! error
    friend int operator~(A, A); //! error
    friend int operator-(A, ...); //! error
    friend bool operator!(A, int); //! error
    friend int operator+(); //! error
    friend int operator*(A, A, A); //! error
    friend int operator&(A, A, int); //! error
    friend A& operator++(); //! error
    friend A& operator++(A&, int, int); //! error
    friend A& operator--(A&, A); //! error
    friend A& operator+=(A&); //! error
    friend A& operator-=(A&, int, int); //! error
    friend A& operator<<=(); //! error
    friend A& operator|=(A&) { throw; } //! error
};
