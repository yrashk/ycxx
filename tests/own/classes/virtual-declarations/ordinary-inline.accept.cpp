constexpr int read_value(const int& value) {
    return value;
}

int read_value(int&) = delete;

struct Inline {
    int stored = 17;

    constexpr virtual int value() const {
        return read_value(stored);
    }
};

const Inline& pass_reference(const Inline& value) {
    return value;
}

int main() {
    return 0;
}
