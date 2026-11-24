// [expr.const.core], [expr.context]
struct Record {
    int value;

    static consteval int get() { return 7; }
};

int check_reference_receiver(Record& input) {
    Record local{19};
    if (input.get() != 7)
        return 1;
    if ((true ? input : local).get() != 7)
        return 2;
    if ((false ? input : local).get() != 7)
        return 3;
    return 0;
}

int main() {
    Record input{42};
    return check_reference_receiver(input);
}
