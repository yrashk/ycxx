// #306 [class.mfct]/[basic.lookup.unqual]: a member of a nested class defined out of class finds enclosing-class names.
struct E {
    static const int k = 7;
    enum { Z = 2 };
    typedef short T;
    struct N { int f(); T g(); };
};
int E::N::f() { return k + Z; }
E::T E::N::g() { T t = 4; return t; }
int main() { E::N n; return (n.f() == 9 && n.g() == 4) ? 0 : 1; }
