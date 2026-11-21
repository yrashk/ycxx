struct Gauge {
    int seed;
    virtual int anchor(int) const;
    virtual int measure(int) const;
};

int check_dispatch(Gauge const& gauge, int expected_anchor, int expected_measure) {
    if (gauge.anchor(3) != expected_anchor)
        return 1;
    if (gauge.measure(3) != expected_measure)
        return 2;
    return 0;
}

int call_base_measure(Gauge const& gauge) {
    return gauge.Gauge::measure(3);
}
