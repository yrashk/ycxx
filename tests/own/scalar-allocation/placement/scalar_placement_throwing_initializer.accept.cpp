using Size = decltype(sizeof(0));

constexpr void* operator new(Size, void* pointer) noexcept {
    return pointer;
}

int initial_value();

int* place(void* address) {
    return new (address) int(initial_value());
}
