struct L {
    int value;
    L& operator=(const L& source) {
        value = source.value + 1;
        return *this;
    }
};

struct H {
    L leaf;
};

int main() {
    H destination{{1}}, source{{4}};
    auto copy = [](H& left, const H& right) {
        left = right;
    };
    copy(destination, source);
    return destination.leaf.value == 5 ? 0 : 1;
}
