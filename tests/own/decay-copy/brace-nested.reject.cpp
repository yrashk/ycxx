// A nested braced-init-list is not an expression ([expr.type.conv], #286).
void f() {
    auto{{1}}; //! error
}
