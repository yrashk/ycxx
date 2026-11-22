// [expr.prim.id.general] permits an objectless data member in an unevaluated operand.
struct Owner {
    unsigned short member;
    enum Result { width = sizeof(member) };
};

static_assert(Owner::width == sizeof(unsigned short));
