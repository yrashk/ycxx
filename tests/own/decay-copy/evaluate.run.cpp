// [expr.type.conv], [conv.lval], [conv.array], [conv.func] (#286).
int next_calls;
int source = 17;

const int& next() {
    ++next_calls;
    return source;
}

int add_one(int value) {
    return value + 1;
}

int main() {
    int first = auto(next());
    if (next_calls != 1 || first != 17) return 1;

    source = 29;
    int second = auto{next()};
    if (next_calls != 2 || first != 17 || second != 29) return 2;

    const int numbers[] = {3, 5};
    if (auto(numbers) != &numbers[0] || auto{numbers}[1] != 5) return 3;
    if (auto(add_one)(9) != 10 || auto{add_one}(11) != 12) return 4;

    return 0;
}
