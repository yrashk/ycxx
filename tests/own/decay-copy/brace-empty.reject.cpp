// A placeholder conversion requires exactly one expression ([expr.type.conv], #286).
void f() {
    auto{}; //! error
}
