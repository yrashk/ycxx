// [expr.type.conv], [dcl.init.general]: empty parentheses value-initialize the result.
struct Beacon {
    Beacon();
    Beacon(const Beacon&) = delete;
    virtual ~Beacon();
};

template<class Factory>
int count_factory(Factory&&) { return 5; }

template<class Result>
int send_factory() {
    return count_factory([] { return Result(); });
}

int count_beacons() { return send_factory<Beacon>(); }
