int add_four(int value) noexcept {
    return value + 4;
}

using Signature = int (*)(int);
constexpr Signature converted = &add_four;

template<Signature Implementation>
int dispatch(int value) {
    return Implementation(value);
}

int main() {
    return dispatch<converted>(8) == 12 ? 0 : 1;
}
