using Size = decltype(sizeof(0));

extern "C" void release_from_peer(int selector);

namespace {
constexpr int thrown_value = 73;
int operand_calls = 0;
int last_selector = -1;
int deallocation_calls = 0;
}

int* peer_operand(int selector) {
    ++operand_calls;
    last_selector = selector;
    if (selector == 0)
        return nullptr;
    throw thrown_value;
}

void operator delete[](void* pointer) noexcept {
    ++deallocation_calls;
    ::operator delete(pointer);
}

void operator delete[](void* pointer, Size) noexcept {
    ++deallocation_calls;
    ::operator delete(pointer);
}

int main() {
    try {
        release_from_peer(9);
        return 1;
    } catch (int value) {
        if (value != thrown_value)
            return 2;
    } catch (...) {
        return 3;
    }

    if (operand_calls != 1 || last_selector != 9)
        return 4;
    if (deallocation_calls != 0)
        return 5;

    release_from_peer(0);
    return operand_calls == 2 && last_selector == 0 ? 0 : 6;
}
