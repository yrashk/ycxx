// #1178, [class.friend]/6: a friend definition has an unqualified name.
struct A;
int operator~(const A&);
A& operator+=(A&, int);
struct A {
    friend int ::operator~(const A&) { return 1; } //! error
    friend A& ::operator+=(A& a, int) { return a; } //! error
};
