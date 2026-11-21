struct Token;

extern "C" void release_int_array(int* pointer) {
    delete[] pointer;
}

extern "C" void release_pointer_array(Token** pointer) {
    delete[] pointer;
}
