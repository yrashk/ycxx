int add_three(int value) {
    return value + 3;
}

int triple(int value) {
    return value * 3;
}

template<int (*Implementation)(int)>
int dispatch(int value) {
    return Implementation(value);
}

int main() {
    if (dispatch<add_three>(5) != 8)
        return 1;
    if (dispatch<&add_three>(5) != 8)
        return 2;
    if (dispatch<triple>(5) != 15)
        return 3;
    if (dispatch<&triple>(5) != 15)
        return 4;
    return 0;
}
