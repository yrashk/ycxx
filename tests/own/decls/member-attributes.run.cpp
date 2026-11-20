// [class.mem.general]: the member declaration may start with an attribute-specifier-seq.
// EXIT: 0
class Reading {
public:
    [[nodiscard("use the reading")]] int value() const;
    [[maybe_unused]] int data;
};

int Reading::value() const {
    return data + 1;
}

int main() {
    const Reading reading{41};
    return reading.value() == 42 && reading.data == 41 ? 0 : 1;
}
