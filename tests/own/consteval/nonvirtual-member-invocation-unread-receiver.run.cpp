// [expr.const.core], [expr.call], [expr.ref]
struct Instance {
    int value;

    consteval int get() const { return 7; }
};

int read_without_object_state(Instance object) {
    return object.get();
}

struct Static {
    int value;

    static consteval int get() { return 11; }
};

int main() {
    if (read_without_object_state({42}) != 7)
        return 1;

    Static first{42};
    Static second{19};
    if (first.get() != 11)
        return 2;
    if ((static_cast<void>(0), first).get() != 11)
        return 3;
    if ((true ? first : second).get() != 11)
        return 4;
    return 0;
}
