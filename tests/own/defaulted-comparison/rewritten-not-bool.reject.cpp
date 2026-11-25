// [over.match.oper]: a rewritten `operator==` candidate selected for `!=` returns cv `bool`.
struct O {
    int v;
    friend int operator==(O, O) { return 1; }
};

bool f(O a, O b) {
    return a != b;  //! error
}
