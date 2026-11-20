struct Reading {
    constexpr friend int first(const Reading*) { return 13; }
    friend constexpr int second(const Reading*) { return 17; }

    inline friend int third(const Reading&) { return 23; }
    friend inline int fourth(const Reading&) { return 29; }
};

static_assert(first(static_cast<const Reading*>(nullptr)) == 13);
static_assert(second(static_cast<const Reading*>(nullptr)) == 17);

int read(const Reading& value) {
    return third(value) + fourth(value);
}
