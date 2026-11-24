// [over.match.oper]: an `operator==` is not a rewrite target when a corresponding `operator!=`
// is declared in the same scope, so `0 != a` has no candidate with the operands reversed.
struct A {};
bool operator==(A, int);
bool operator!=(A, int);

bool f(A a) {
    return 0 != a;  //! error
}
