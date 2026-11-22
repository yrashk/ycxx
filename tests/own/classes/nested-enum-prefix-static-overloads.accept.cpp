// [class.member.lookup] supplies the preceding declarations to [over.match].
struct Owner {
    static int f(int);
    static long long f(long);
    enum Result { narrow = sizeof(f(0)), wide = sizeof(f(0L)) };
};

static_assert(Owner::narrow == sizeof(int));
static_assert(Owner::wide == sizeof(long long));

struct LaterOverload {
    static int f(long);
    enum Result { width = sizeof(f(0)) };
    static long long f(int);
};

static_assert(LaterOverload::width == sizeof(int));
