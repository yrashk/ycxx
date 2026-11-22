using Size = decltype(sizeof(0));

constexpr void* operator new(Size, void* pointer) noexcept {
    return pointer;
}

int storage = 0;
int calls = 0;

int* placed = new (&storage) int((calls = calls + 1, 41));

int main() {
    if (placed != &storage || *placed != 41 || storage != 41)
        return 1;
    return calls == 1 ? 0 : 2;
}
