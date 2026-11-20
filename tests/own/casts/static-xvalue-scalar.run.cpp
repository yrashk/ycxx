// [expr.static.cast], [dcl.init.ref], [basic.lval] (#199).
int category(int&) { return 11; }
int category(int&&) { return 12; }
int category(const int&) { return 13; }
int category(const int&&) { return 14; }

void write_value(int&& target, int value) {
    target = value;
}

int main() {
    int object = 17;
    if (category(object) != 11 || category(static_cast<int&&>(object)) != 12) return 1;

    decltype(static_cast<int&&>(object)) alias = static_cast<int&&>(object);
    if (&alias != &object || category(alias) != 11) return 2;

    write_value(static_cast<int&&>(object), 23);
    if (object != 23 || alias != 23) return 3;

    const int& const_view = object;
    const int&& const_alias = static_cast<const int&&>(const_view);
    if (&const_alias != &object || category(const_alias) != 13 ||
        category(static_cast<const int&&>(const_view)) != 14 ||
        category(static_cast<const int&&>(object)) != 14) {
        return 4;
    }

    alias = 29;
    if (object != 29 || const_alias != 29) return 5;

    volatile int volatile_object = 31;
    volatile int&& volatile_alias = static_cast<volatile int&&>(volatile_object);
    const volatile int&& cv_alias = static_cast<const volatile int&&>(volatile_object);
    if (&volatile_alias != &volatile_object || &cv_alias != &volatile_object) return 6;
    volatile_alias = 37;
    if (volatile_object != 37 || cv_alias != 37) return 7;
    return 0;
}
