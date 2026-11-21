using Size = decltype(sizeof(0));

constexpr void* operator new(Size, void* pointer) noexcept {
    return pointer;
}

constexpr void* operator new[](Size, void* pointer) noexcept {
    return pointer;
}

constexpr void operator delete(void*, void*) noexcept {}

constexpr void operator delete[](void*, void*) noexcept {}

static_assert(::operator new(0, nullptr) == nullptr);
static_assert(::operator new[](0, nullptr) == nullptr);

constexpr bool direct_placement_calls() {
    int storage = 0;
    void* pointer = &storage;

    ::operator delete(nullptr, nullptr);
    ::operator delete[](nullptr, nullptr);

    return ::operator new(0, pointer) == pointer &&
           ::operator new[](0, pointer) == pointer;
}

static_assert(direct_placement_calls());

int main() {
    int storage = 0;
    void* pointer = &storage;
    int size_evaluations = 0;
    int pointer_evaluations = 0;

    void* result = ::operator new((++size_evaluations, 0),
                                  (++pointer_evaluations, pointer));

    if (result != pointer)
        return 1;
    if (size_evaluations != 1)
        return 2;
    if (pointer_evaluations != 1)
        return 3;
    return 0;
}
