// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    constexpr virtual ~Root() {}
    constexpr virtual int value() const { return 17; }
};

constexpr int evaluate() {
    Root object;
    const Root& reference = object;
    return reference.value();
}

static_assert(evaluate() == 17);

int main() {
    return evaluate() != 17;
}
