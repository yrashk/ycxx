// [expr.static.cast], [dcl.init.ref], [basic.lval] (#199).
struct Base {
    int value;
};

struct Derived : Base {
    int other;
};

int category(Base&) { return 11; }
int category(Base&&) { return 12; }

int main() {
    Derived object{};
    object.value = 17;
    object.other = 19;

    Derived&& same_object = static_cast<Derived&&>(object);
    if (&same_object != &object) return 1;

    Base* expected = &object;
    Base&& base = static_cast<Base&&>(object);
    if (&base != expected || category(base) != 11 ||
        category(static_cast<Base&&>(object)) != 12) {
        return 2;
    }

    base.value = 23;
    same_object.other = 29;
    if (object.value != 23 || object.other != 29) return 3;
    return 0;
}
