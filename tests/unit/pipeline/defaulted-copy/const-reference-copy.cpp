struct Bound {
    const int value;
    int& reference;

    Bound(int input, int& target) noexcept : value(input), reference(target) {}
    constexpr Bound(const Bound&) noexcept = default;
};

int main() {
    int target = 17;
    const Bound source(23, target);
    Bound copy(source);
    copy.reference = 31;
    return copy.value == 23 && &copy.reference == &target && target == 31 ? 0 : 1;
}
