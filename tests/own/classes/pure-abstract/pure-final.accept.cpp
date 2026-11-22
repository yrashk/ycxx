struct Closed {
    virtual int score(int) const final = 0;
};

struct Leaf : Closed {
    int score(long) const;
};

static_assert(__is_abstract(Closed));
static_assert(__is_abstract(Leaf));
