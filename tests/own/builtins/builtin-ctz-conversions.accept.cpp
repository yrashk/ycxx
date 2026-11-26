// Argument conversion to the GNU builtin's unsigned int parameter.
struct Convertible {
    constexpr operator unsigned int() const { return 64u; }
};

constexpr int count_after_truncation(double value) {
    return __builtin_ctz(value);
}

static_assert(__builtin_ctz(Convertible{}) == 6);
static_assert(count_after_truncation(8.75) == 3);
