struct A { int n; };

struct StaticAggregate {
    long long f(long*);
    static int f(A);
    enum Result { width = sizeof(f({1})) };
};

struct StaticPointer {
    int f(A);
    static long long f(long*);
    enum Result { width = sizeof(f({nullptr})) };
};

static_assert(StaticAggregate::width == sizeof(int));
static_assert(StaticPointer::width == sizeof(long long));
