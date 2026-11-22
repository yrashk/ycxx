struct Source {
    operator int() const & { return 21; }
    operator int() const && { return 42; }
};

int main() {
    const Source source{};
    return static_cast<int>(source) != 21 ||
           static_cast<int>(static_cast<const Source&&>(source)) != 42;
}
