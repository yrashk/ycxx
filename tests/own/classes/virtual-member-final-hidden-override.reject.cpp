// [class.virtual]/4: overriding a final virtual is ill-formed.
class Far {
    virtual int score(int) const final;
};

struct Near : Far {
    int score(long) const;
};

struct Leaf : Near {
    int score(int) const;  //! error
};
