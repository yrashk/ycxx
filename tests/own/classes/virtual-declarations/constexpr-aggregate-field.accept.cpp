// XFAIL: ycxx #25 constant evaluation of aggregate field access
struct Aggregate {
    int value;
};

constexpr Aggregate aggregate{17};
static_assert(aggregate.value == 17);
