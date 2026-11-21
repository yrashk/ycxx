using Size = decltype(sizeof(0));

constexpr void* operator new(Size, void* pointer) noexcept {
    return pointer;
}

int* place(void* pointer) {
    return ::new (pointer) (int)(7);
}
