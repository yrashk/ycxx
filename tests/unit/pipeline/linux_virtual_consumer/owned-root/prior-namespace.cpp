namespace [[gnu::visibility("hidden")]] devices {
    struct Gauge;
}
namespace devices {
    struct Gauge {
        int seed;
        virtual int anchor(int) const;
        virtual unsigned measure(unsigned) const;
    };
    int Gauge::anchor(int n) const { return seed + n; }
}
