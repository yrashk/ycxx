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

extern "C" int puts(const char*);

static_assert(sizeof(B) == 16 && alignof(B) == 8);
static_assert(sizeof(D) == 16 && alignof(D) == 8);
static_assert(sizeof(Wide) == 24 && alignof(Wide) == 8);

int main() {
    const B* root = peer_base();
    if (root->marker(2) != 9 || root->score(2) != 105) return 1;

    D* derived = peer_derived();
    const D& derived_ref = *derived;
    B* base = derived;
    const B& base_ref = derived_ref;
    if (static_cast<const void*>(derived) != static_cast<const void*>(base)) return 2;
    if (&base_ref != base) return 3;
    D* missing = peer_null();
    B* null_base = missing;
    if (null_base != nullptr) return 4;

    if (derived_ref.base != 5 || derived_ref.extra != 7 || base_ref.base != 5) return 5;
    if (derived_ref.score(3) != 234 || base->score(3) != 234) return 6;
    if (base->B::score(3) != 108) return 7;

    const D& extension = *peer_extension();
    if (extension.score(4) != 365 || extension.D::score(4) != 265) return 8;
    if (peer_next()->score(4) != 365 || peer_calls() != 1) return 9;

    Wide* wide = peer_wide();
    if (wide->base != 23 || wide->extra != 0x100000009ULL) return 10;
    if (wide->score(3) != 427) return 11;
    if (!peer_layout(&derived_ref.base, &derived_ref.extra, &wide->base, &wide->extra)) return 12;

    derived->base = 17;
    derived->extra = 19;
    wide->extra = 0x20000000bULL;
    if (base_ref.base != 17 || !peer_observed_writes()) return 13;

    puts("primary-base consumer ok");
    return 0;
}
