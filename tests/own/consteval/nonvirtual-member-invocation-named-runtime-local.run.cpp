// [expr.const.core], [expr.call]
struct Record {
    int value;

    consteval int get() const { return 7; }
};

int call_on_local(int value) {
    Record record{value};
    return record.get();
}

int call_on_parenthesized_local(int value) {
    Record record{value};
    return (record).get();
}

int main() {
    if (call_on_local(42) != 7)
        return 1;
    if (call_on_parenthesized_local(19) != 7)
        return 2;
    return 0;
}
