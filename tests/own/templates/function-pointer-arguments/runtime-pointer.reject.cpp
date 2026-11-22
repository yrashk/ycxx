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

int (*runtime_pointer)(int) = add_three;

int invalid() {
    return dispatch<runtime_pointer>(5);  //! error
}
