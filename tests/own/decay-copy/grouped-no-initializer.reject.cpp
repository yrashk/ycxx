// [stmt.ambig]
int value;
void declaration() {
    auto(value); //! error
}
