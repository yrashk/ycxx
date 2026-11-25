// #1178, [over.assign], [over.call], [over.sub], [over.ref]: operators = () [] -> must be member
// functions, so a friend declaration cannot declare them.
struct A {
    friend A& operator=(A&, int); //! error
    friend int operator()(A&); //! error
    friend int operator[](A&, int); //! error
    friend A* operator->(A&); //! error
};
