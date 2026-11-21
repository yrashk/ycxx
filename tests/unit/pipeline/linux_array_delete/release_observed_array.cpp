extern "C" int* observe_array_operand(int* pointer);

extern "C" void release_observed_array(int* pointer) {
    delete[] observe_array_operand(pointer);
}
