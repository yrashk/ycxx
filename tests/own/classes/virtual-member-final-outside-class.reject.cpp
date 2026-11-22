// [class.mem.general]: the virt-specifier belongs to the member declaration.
struct Base {
    virtual int score(int) const;
};

int Base::score(int value) const final { return value; }  //! error
