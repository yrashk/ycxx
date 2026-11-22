// [dcl.fct.def.default], Itanium C++ ABI §2.2 and §2.4
struct Implicit {
    int first;
    char last;
};

struct Defaulted {
    int first;
    char last;
    Defaulted& operator=(const Defaulted&) = default;
};

struct Deleted {
    int first;
    char last;
    Deleted& operator=(const Deleted&) = delete;
};

struct Provided {
    int first;
    char last;
    Provided& operator=(const Provided&) { return *this; }
};

struct Nested {
    Deleted member;
    char last;
    Nested& operator=(const Nested&) = default;
};

struct FromImplicit : Implicit { char extra; };
struct FromDefaulted : Defaulted { char extra; };
struct FromDeleted : Deleted { char extra; };
struct FromProvided : Provided { char extra; };
struct FromNested : Nested { char extra; };

static_assert(sizeof(FromImplicit) == 12);
static_assert(sizeof(FromDefaulted) == sizeof(FromImplicit));
static_assert(sizeof(FromDeleted) == sizeof(FromImplicit));
static_assert(sizeof(FromProvided) == 8);
static_assert(sizeof(FromNested) == 16);
