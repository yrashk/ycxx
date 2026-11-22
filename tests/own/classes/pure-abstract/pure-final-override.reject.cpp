struct Closed {
    virtual int score(int) const final = 0;
};

struct Leaf : Closed {
    int score(int) const;  //! error
};
