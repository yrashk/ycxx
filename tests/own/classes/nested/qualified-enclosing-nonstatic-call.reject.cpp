// #1216 [expr.prim.id.general]/2,4: an unevaluated non-static call still needs an object.
struct Outer {
    int marker();
    struct Inner {
        static constexpr auto value = sizeof(Outer::marker());  //! error
    };
};
