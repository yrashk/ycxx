struct Aggregate {
    int value;
};

constexpr Aggregate aggregate{17};
static_assert(aggregate.value == 17);
