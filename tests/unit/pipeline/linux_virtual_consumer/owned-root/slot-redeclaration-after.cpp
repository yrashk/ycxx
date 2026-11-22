struct Gauge {
    int seed;
    virtual int anchor(int) const;
    virtual unsigned measure(unsigned) const;
};
int Gauge::anchor(int n) const { return seed + n; }
struct Witness {
    friend unsigned Gauge::measure(unsigned) const __attribute__((visibility("hidden")));
};
