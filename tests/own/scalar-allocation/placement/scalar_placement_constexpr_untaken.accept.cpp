using Size = decltype(sizeof(0));

constexpr void* operator new(Size, void* pointer) noexcept {
    return pointer;
}

static_assert(::operator new(0, nullptr) == nullptr);

constexpr int maybe_place(bool take) {
    if (!take)
        return 7;

    int storage = 0;
    return *new (&storage) int(19);
}

static_assert(maybe_place(false) == 7);
