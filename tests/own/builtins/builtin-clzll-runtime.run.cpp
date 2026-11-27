volatile unsigned long long clzll_input = (1ULL << 32) - 1;

int main() {
    unsigned long long value = clzll_input;
    int count = __builtin_clzll(value++);
    return count == 32 && value == (1ULL << 32) ? 0 : 1;
}
