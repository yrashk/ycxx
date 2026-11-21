using Size = decltype(sizeof(0));

int* place(void* address) {
    return new (address) int(9); //! error
}

constexpr void* operator new(Size, void* pointer) noexcept {
    return pointer;
}
