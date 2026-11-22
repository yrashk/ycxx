namespace arithmetic {
int twice(int value) {
    return value * 2;
}

template<int (*Implementation)(int)>
int dispatch(int value) {
    if (Implementation == nullptr)
        return -1;
    return Implementation(value);
}
}

extern "C" int target(int value) {
    return value + 5;
}

using Signature = decltype(&target);

template<Signature Implementation>
int dispatch_c(int value) {
    return Implementation(value);
}

int main() {
    if (arithmetic::dispatch<arithmetic::twice>(3) != 6)
        return 1;
    if (arithmetic::dispatch<nullptr>(3) != -1)
        return 2;
    if (dispatch_c<&target>(3) != 8)
        return 3;
    return 0;
}
