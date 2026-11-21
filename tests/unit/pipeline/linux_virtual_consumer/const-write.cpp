struct B {
    virtual int score(int) const;
    unsigned base;
};

struct D : public B {
    unsigned extra;
    int score(int) const override;
};

void overwrite(const D& object) {
    object.base = 4;
}
