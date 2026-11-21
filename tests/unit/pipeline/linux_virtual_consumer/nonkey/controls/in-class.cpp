struct Gauge {
    int seed;
    virtual int anchor(int) const;
    virtual int measure(int x) const {
        return seed * 7 + x;
    }
};
