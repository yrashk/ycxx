struct Token;

extern "C" int* make_int_array() {
    return new int[3];
}

extern "C" Token** make_pointer_array() {
    return new Token*[2];
}

extern "C" int* make_empty_int_array() {
    return new int[0];
}

extern "C" void release_int_array(int* pointer) {
    delete[] pointer;
}

extern "C" void release_pointer_array(Token** pointer) {
    delete[] pointer;
}
