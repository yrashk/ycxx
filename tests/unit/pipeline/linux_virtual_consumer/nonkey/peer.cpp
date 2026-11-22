struct Gauge {
    int seed;
    virtual int anchor(int) const;
    virtual int measure(int) const;
};

int Gauge::anchor(int x) const {
    return seed - x;
}

struct PeerGauge : Gauge {
    int measure(int) const override;
};

int PeerGauge::measure(int x) const {
    return seed + 11 * x;
}

int check_dispatch(Gauge const&, int expected_anchor, int expected_measure);
int call_base_measure(Gauge const&);

int main() {
    Gauge base;
    base.seed = 5;
    int result = check_dispatch(base, 2, 38);
    if (result != 0)
        return result;

    PeerGauge derived;
    derived.seed = 9;
    result = check_dispatch(derived, 6, 42);
    if (result != 0)
        return result + 2;
    if (call_base_measure(derived) != 66)
        return 5;
    return 0;
}
