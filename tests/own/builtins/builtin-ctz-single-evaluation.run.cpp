// REQUIRES: run
// A nonzero runtime operand and one call to the argument expression.
unsigned int sample(int& calls, int argc) {
    ++calls;
    return argc == 1 ? 40u : 192u;
}

int main(int argc, char**) {
    int calls = 0;
    int result = __builtin_ctz(sample(calls, argc));
    if (calls != 1) return 1;
    return result == (argc == 1 ? 3 : 6) ? 0 : 2;
}
