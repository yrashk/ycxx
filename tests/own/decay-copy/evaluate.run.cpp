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

int left_calls;
int right_calls;

int lookup(int input) {
    ++left_calls;
    return input;
}

int rhs() {
    ++right_calls;
    return 1;
}

void check_binary_statements(int input) {
    auto(lookup)(int(input)) & rhs();
    auto(lookup)(int(input)) && rhs();
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

    check_binary_statements(0);
    if (left_calls != 2 || right_calls != 1) return 5;
    check_binary_statements(3);
    if (left_calls != 4 || right_calls != 3) return 6;

    return 0;
}
