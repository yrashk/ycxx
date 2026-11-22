struct Source {
    operator int() & { return 21; }
    operator int() && { return 42; }
};

int main() {
    Source source;
    return static_cast<int>(source) != 21 ||
           static_cast<int>(static_cast<Source&&>(source)) != 42;
}
