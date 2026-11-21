struct B {
    virtual int marker(int) const;
    virtual int score(int) const;
    unsigned base;
};

struct D : public B {
    unsigned extra;
    int score(int) const override;
};

struct Wide : public B {
    unsigned long long extra;
    int score(int) const override;
};

extern "C" B* peer_base();
extern "C" D* peer_derived();
extern "C" D* peer_null();
extern "C" const D* peer_extension();
extern "C" const D* peer_next();
extern "C" unsigned peer_calls();
extern "C" Wide* peer_wide();
extern "C" int peer_layout(const unsigned*, const unsigned*, const unsigned*,
                           const unsigned long long*);
extern "C" int peer_observed_writes();

struct E : public D {
    int score(int) const override;
};

int B::marker(int n) const {
    return 7 + n;
}

int B::score(int n) const {
    return 100 + static_cast<int>(base) + n;
}

int D::score(int n) const {
    return 200 + 2 * static_cast<int>(base) + 3 * static_cast<int>(extra) + n;
}

int E::score(int n) const {
    return 300 + 2 * static_cast<int>(base) + 3 * static_cast<int>(extra) + n;
}

int Wide::score(int n) const {
    return 400 + static_cast<int>(base) + static_cast<int>(extra >> 32) + n;
}

namespace {
constinit B root = [] {
    B object;
    object.base = 3;
    return object;
}();

constinit D derived = [] {
    D object;
    object.base = 5;
    object.extra = 7;
    return object;
}();

constinit E extension = [] {
    E object;
    object.base = 11;
    object.extra = 13;
    return object;
}();

constinit Wide wide = [] {
    Wide object;
    object.base = 23;
    object.extra = 0x100000009ULL;
    return object;
}();

unsigned receiver_calls;
}

extern "C" B* peer_base() { return &root; }
extern "C" D* peer_derived() { return &derived; }
extern "C" D* peer_null() { return nullptr; }
extern "C" const D* peer_extension() { return &extension; }
extern "C" unsigned peer_calls() { return receiver_calls; }
extern "C" Wide* peer_wide() { return &wide; }

extern "C" const D* peer_next() {
    ++receiver_calls;
    return &extension;
}

extern "C" int peer_layout(const unsigned* base, const unsigned* extra,
                           const unsigned* wide_base, const unsigned long long* wide_extra) {
    // Check actual addresses in the peer's live objects. Linux x86-64 unsigned
    // long can represent these addresses, and its ABI uses byte addresses.
    return base == &derived.base && extra == &derived.extra
        && wide_base == &wide.base && wide_extra == &wide.extra
        && reinterpret_cast<unsigned long>(base) - reinterpret_cast<unsigned long>(&derived) == 8
        && reinterpret_cast<unsigned long>(extra) - reinterpret_cast<unsigned long>(&derived) == 12
        && reinterpret_cast<unsigned long>(wide_base) - reinterpret_cast<unsigned long>(&wide) == 8
        && reinterpret_cast<unsigned long>(wide_extra) - reinterpret_cast<unsigned long>(&wide) == 16;
}

extern "C" int peer_observed_writes() {
    return derived.base == 17 && derived.extra == 19 && wide.extra == 0x20000000bULL
        && wide.base == 23 && root.base == 3 && extension.base == 11 && extension.extra == 13;
}
