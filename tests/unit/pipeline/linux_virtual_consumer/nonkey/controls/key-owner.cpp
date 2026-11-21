struct Gauge {
    int seed;
    virtual int anchor(int) const;
    virtual int measure(int) const;
};

int Gauge::anchor(int x) const {
    return seed - x;
}
