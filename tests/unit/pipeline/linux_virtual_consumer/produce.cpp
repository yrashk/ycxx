struct B {
    virtual int score(int) const;
    unsigned base;
};

struct D : public B {
    unsigned extra;
    int score(int) const override;
};

D produce() {
    return D{};
}
