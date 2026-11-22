// [class.abstract] determines abstractness through each pure virtual's final overrider.
#define PURE = 0
#define ZERO 0
struct Root {
    virtual int score(int) const PURE;
    virtual int score(long) = ZERO;
};
#undef ZERO
#undef PURE

struct Hidden : Root {
    int score(double) const;
};

struct Partial : Hidden {
    int score(int) const override;
};

struct Concrete : Partial {
    int score(long) override;
};

struct PureAgain : Concrete {
    int score(int) const override = 0;
};

struct ConcreteAgain : PureAgain {
    int score(int) const override;
};

static_assert(__is_abstract(Root));
static_assert(__is_abstract(Hidden));
static_assert(__is_abstract(Partial));
static_assert(!__is_abstract(Concrete));
static_assert(__is_abstract(PureAgain));
static_assert(!__is_abstract(ConcreteAgain));
