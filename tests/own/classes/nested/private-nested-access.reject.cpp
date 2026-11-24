// #306 [class.access.nest], [class.access]/1: a private nested class cannot be named outside the enclosing class.
class O {
    struct Hidden { int x; };
public:
    int f() { Hidden h{1}; return h.x; }
};
O::Hidden h;  //! error
