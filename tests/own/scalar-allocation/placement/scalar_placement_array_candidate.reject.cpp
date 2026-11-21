using Size = decltype(sizeof(0));

constexpr void* operator new[](Size, void* pointer) noexcept {
    return pointer;
}

int* place(void* address) {
    return new (address) int(9); //! error
}
