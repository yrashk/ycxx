// [stmt.ambig], [expr.type.conv]
int source(int);

int discarded(int value) {
    auto(value++);
    auto((value += 2, ++value));
    auto(value + 1);
    if (auto(source)(value)) return value;
    if (auto(source)(value) != 0 || auto{source}(value) != 0) return value;
    return 0;
}

int declarations() {
    auto(source)(7);
    int* pointer = &source;
    auto(value) = 41;
    return *pointer + value;
}

int subscripts(int* pointer) {
    if (auto(pointer)[0] != 0 || auto{pointer}[1] != 0) return 1;
    if (auto(value) = *pointer) return value;
    return 0;
}
