#ifndef ABI309_VIRTUAL_MEMBER_FINAL_HPP
#define ABI309_VIRTUAL_MEMBER_FINAL_HPP

namespace final_probe {
struct B {
    virtual int marker(int) const;
    virtual int score(int) const;
    int tag;
};

struct D : B {
    int score(int) const final override;
};

int base_slot(const B* object, int value);
int derived_slot(const D& object, int value);
int qualified_base(const B& object, int value);
int other_slot(const B* object, int value);
}

#endif
