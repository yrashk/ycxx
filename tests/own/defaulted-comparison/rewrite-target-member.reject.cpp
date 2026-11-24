// [over.match.oper]: a member `operator==` is not a rewrite target when its class declares a
// corresponding `operator!=`.
struct A {
    bool operator==(int) const;
    bool operator!=(int) const;
};

bool f(A a) {
    return 0 == a;  //! error
}
