// [class.mem.general]: each virt-specifier can appear at most once.
struct Base {
    virtual int score(int) const;
};

struct Derived : Base {
    int score(int) const final override final;  //! error
};
