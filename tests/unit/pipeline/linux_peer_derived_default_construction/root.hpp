#ifndef ABI309_PEER_DERIVED_HPP
#define ABI309_PEER_DERIVED_HPP

struct B {
    virtual int f() = 0;
};

struct D : public B {
    int tag;
    int f() override;
};

extern "C" int make_derived(int initial, int updated) noexcept;
extern "C" int peer_observe(D& object, B& base, int initial, int updated) noexcept;
int opaque_dispatch(B& base);

#endif
