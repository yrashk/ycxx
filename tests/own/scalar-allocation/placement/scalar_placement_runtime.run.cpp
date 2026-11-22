using Size = decltype(sizeof(0));

constexpr void* operator new(Size, void* pointer) noexcept {
    return pointer;
}

void* placement_address(int* storage, int* phase, int* calls) noexcept {
    ++*calls;
    *phase = *phase * 10 + 1;
    return storage;
}

int main() {
    int storage = 0;
    int phase = 0;
    int calls = 0;

    int* placed = new (placement_address(&storage, &phase, &calls))
        int((phase = phase * 10 + 2, 73));
    if (placed != &storage)
        return 1;
    if (*placed != 73 || storage != 73)
        return 2;
    if (phase != 12)
        return 3;
    if (calls != 1)
        return 4;
    return 0;
}
