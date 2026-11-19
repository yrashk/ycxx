// ζ c6 (#88): outside a templated entity a requires-expression's requirements are checked as
// written, and an invalid one is ill-formed rather than false ([expr.prim.req.general]/5).
int x = 0;
static_assert(!requires { x.member; }); //! error
int main() { return 0; }
