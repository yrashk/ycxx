// [over.call.func] allows the call when overload resolution selects the static member.
struct StaticFirst {
    static int f(int);
    long long f(long);
    enum Result { width = sizeof(f(0)) };
};

struct StaticLast {
    long long f(long);
    static int f(int);
    enum Result { width = sizeof(f(0)) };
};

static_assert(StaticFirst::width == sizeof(int));
static_assert(StaticLast::width == sizeof(int));
