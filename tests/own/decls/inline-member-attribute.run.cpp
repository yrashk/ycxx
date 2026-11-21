// [class.mem.general]: attributes may precede an inline member function definition.
// EXIT: 0
struct Buffer {
    int size;

    [[nodiscard]] [[maybe_unused]] constexpr bool empty() const noexcept {
        return size == 0;
    }
};

int main(int argc, char**) {
    const Buffer buffer{argc - 1};
    return buffer.empty() == (argc == 1) ? 0 : 1;
}
