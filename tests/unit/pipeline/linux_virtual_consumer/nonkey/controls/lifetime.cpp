struct Gauge {
    int seed;
    virtual int anchor(int) const;
    virtual int measure(int) const;
};

int local_gauge_seed() {
    Gauge local;
    local.seed = 5;
    return local.seed;
}
