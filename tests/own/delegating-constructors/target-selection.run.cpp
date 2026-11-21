struct C {
    int value;

    C(int value) : C(value, 0) {}
    C(char value) : C{value, 0L} {}
    C();

    C(int value, int extra);
    C(int value, long extra);
    C(long value, int extra = 40);
};

C::C() : C(7L) {}

C::C(int value, int extra) : value(value + extra + 10) {}

C::C(int value, long extra) : value(value + extra + 20) {}

C::C(long value, int extra) : value(value + extra) {}

int main() {
    C parens(7);
    C braces('a');
    C defaults;
    if (parens.value != 17)
        return 4;
    if (braces.value != 'a' + 20)
        return 5;
    if (defaults.value != 47)
        return 6;
}
