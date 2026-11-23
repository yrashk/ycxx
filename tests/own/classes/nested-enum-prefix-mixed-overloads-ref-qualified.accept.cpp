struct Owner {
    static int f(int);
    virtual long f(long) &;
    enum Result { width = sizeof(f(0)) };
};

static_assert(Owner::width == sizeof(int));
