// #119, from the review: a qualified friend must name an earlier declaration
// ([namespace.memdef]/3), and a function that is not a friend gets no access to the class's
// private types in its declaration ([class.access]/4).
class S { using T = int; friend int ::nowhere(S&); };  //! error
class U { using T = int; T x = 3; friend T look(U&); };
U::T look(U& u) { return u.x; }
U::T other(U&) { return 0; }  //! error
int main() { return 0; }
