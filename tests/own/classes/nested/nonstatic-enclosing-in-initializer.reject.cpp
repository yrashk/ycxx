// #306 [class.nest] example, [expr.prim.id]/3: a non-static enclosing member in a default member initializer of a nested class.
struct E {
    int i;
    struct N {
        int a = i;  //! error
    };
};
