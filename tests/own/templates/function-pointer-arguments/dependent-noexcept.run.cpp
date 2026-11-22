using Signature = int (*)(int) noexcept;

int add_two(int value) noexcept {
    return value + 2;
}

int subtract_two(int value) noexcept {
    return value - 2;
}

template<class FunctionPointer, FunctionPointer Implementation>
int dispatch(int value) {
    return Implementation(value);
}

int main() {
    if (dispatch<Signature, add_two>(7) != 9)
        return 1;
    if (dispatch<Signature, &subtract_two>(7) != 5)
        return 2;
    return 0;
}
