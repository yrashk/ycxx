// #1178, [over.oper.general]: a nonmember operator function has a parameter of class or
// enumeration type or a reference to one.
struct A {
    friend int operator~(int); //! error
    friend bool operator!(bool b) { return b; } //! error
    friend int operator+(const double&); //! error
    friend int& operator++(int&); //! error
    friend int operator*(A*); //! error
    friend A operator&(A*&); //! error
    friend int& operator+=(int&, int); //! error
    friend A* operator-=(A*, int) { return nullptr; } //! error
    friend A& operator^=(const char*, A*); //! error
};
