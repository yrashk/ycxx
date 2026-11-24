// #306 [class.nest] example, [expr.prim.id]/3: a non-static enclosing member used without an object in an evaluated context is ill-formed.
struct E {
    int i;
    struct N {
        int f() { return i; }  //! error
    };
};
