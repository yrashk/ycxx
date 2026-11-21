struct Root {
    virtual int value() requires true { return 0; } //! error
};
