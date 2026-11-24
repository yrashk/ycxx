// #306 [class.mem]/[basic.def.odr]: a nested class may not be defined twice.
struct S {
    struct In { int a; };
    struct In { int b; };  //! error
};
