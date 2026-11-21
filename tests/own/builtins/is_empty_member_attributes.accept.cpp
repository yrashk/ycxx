struct Empty {};
struct Payload {
    int value;
};

struct AttributedPayload {
    [[no_unique_address]] Payload member;
};

struct [[gnu::packed]] PackedOrdinary {
    Empty member;
};

struct Mixed {
    [[no_unique_address]] Empty overlap;
    Empty ordinary;
};

static_assert(!__is_empty(AttributedPayload));
static_assert(!__is_empty(PackedOrdinary));
static_assert(!__is_empty(Mixed));
