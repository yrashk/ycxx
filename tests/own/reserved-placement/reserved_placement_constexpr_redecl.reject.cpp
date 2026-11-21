using Size = decltype(sizeof(0));

constexpr void* operator new(Size, void* pointer) noexcept {
    return pointer;
}

void* operator new(Size, void*) noexcept; //! error
