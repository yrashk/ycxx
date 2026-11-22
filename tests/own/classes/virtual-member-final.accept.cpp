// [class.virtual], [class.mem.general]: final applies to virtual members.
struct Base {
    virtual int score(int) const;
    virtual int final(int) final;
};

struct OverrideFirst : Base {
    int score(int) const override final;
};

struct FinalFirst : Base {
    int score(int) const final override;
};

struct Named {
    int final;
};

int final(const Named& object, int final) {
    return object.final + final;
}

class Far {
    virtual int score(int) const final;
};

struct Near : Far {
    int score(long) const;
};

struct Different : Near {
    int score(double) const;
    int score(int);
};
