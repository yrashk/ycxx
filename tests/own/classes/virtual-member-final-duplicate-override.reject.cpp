// [class.mem.general]: final does not change the limit on override specifiers.
struct Base {
    virtual int score(int) const;
};

struct Derived : Base {
    int score(int) const override final override;  //! error
};
