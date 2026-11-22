// [expr.static.cast], [conv.ptr], [class.access.base] (#199).
struct Lead {
    int tag;
};

struct Base {
    int value;
};

struct Derived : Lead, Base {
    int other;
};

int calls;

Base&& get_base(Base& base) {
    ++calls;
    return static_cast<Base&&>(base);
}

int category(Derived&) { return 1; }
int category(Derived&&) { return 2; }

class PrivateDerived : Lead, Base {
public:
    static bool check(PrivateDerived& object) {
        ::Base& base = object;
        PrivateDerived&& alias = static_cast<PrivateDerived&&>(static_cast<::Base&&>(base));
        return &alias == &object;
    }
};

int main() {
    Derived object{};
    Base* base = &object;
    if (static_cast<void*>(base) == static_cast<void*>(&object)) return 1;

    Derived&& alias = static_cast<Derived&&>(get_base(*base));
    if (&alias != &object || calls != 1) return 2;
    alias.value = 17;
    alias.other = 23;
    if (base->value != 17 || object.other != 23) return 3;
    if (category(static_cast<Derived&&>(get_base(*base))) != 2 || calls != 2) return 4;

    const Base& const_base = *base;
    const volatile Derived&& cv_alias =
        static_cast<const volatile Derived&&>(static_cast<const Base&&>(const_base));
    if (&cv_alias != &object) return 5;

    PrivateDerived private_object{};
    if (!PrivateDerived::check(private_object)) return 6;
    return 0;
}
