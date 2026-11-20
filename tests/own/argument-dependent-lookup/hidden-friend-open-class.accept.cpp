struct Declared {
    friend int before_alias(Declared*);
    using Result = decltype(before_alias(static_cast<Declared*>(nullptr)));
};

struct Defined {
    friend int before_alias(Defined*) { return 17; }
    using Result = decltype(before_alias(static_cast<Defined*>(nullptr)));

    int method() { return after_method(this); }
    friend int after_method(Defined*) { return 29; }
};
