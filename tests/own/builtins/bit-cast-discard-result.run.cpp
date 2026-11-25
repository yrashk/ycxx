struct Result { unsigned value; };

constexpr unsigned discarded_comma(unsigned input) {
    (void)(++input, __builtin_bit_cast(Result, input));
    return input;
}

constexpr unsigned discarded_choice(unsigned input, bool left) {
    (void)(left ? __builtin_bit_cast(Result, ++input) : Result{input += 2});
    return input;
}

static_assert(discarded_comma(1) == 2);
static_assert(discarded_choice(1, true) == 2);
static_assert(discarded_choice(1, false) == 3);

int main(int argc, char**) {
    const auto input = static_cast<unsigned>(argc);
    if (discarded_comma(input) != input + 1) return 1;
    if (discarded_choice(input, (input & 1u) != 0) != input + ((input & 1u) != 0 ? 1 : 2)) return 2;
    return 0;
}
