// [stmt.ambig], [expr.type.conv]
int source(int);

int discarded(int value) {
    auto(value++);
    auto((value += 2, ++value));
    auto(value + 1);
    if (auto(source)(value)) return value;
    return 0;
}

int declarations() {
    auto(source)(7);
    int* pointer = &source;
    auto(value) = 41;
    return *pointer + value;
}
