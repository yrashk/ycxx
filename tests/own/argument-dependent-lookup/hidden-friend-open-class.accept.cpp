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

struct Deduced {
    friend auto completed(Deduced*) { return 37; }
};

using Completed = decltype(completed(static_cast<Deduced*>(nullptr)));
static_assert(static_cast<Completed>(37) == 37);
