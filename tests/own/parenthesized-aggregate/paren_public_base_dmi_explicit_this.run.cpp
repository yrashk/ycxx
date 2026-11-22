struct B {
    int base_value;
};

struct D : public B {
    int member = this->base_value + 7;
};

int main() {
    B source{17};
    D named(source);
    D temporary(B{31});
    D supplied(B{43}, 61);

    if (named.base_value != 17 || named.member != 24)
        return 1;
    if (temporary.base_value != 31 || temporary.member != 38)
        return 2;
    if (supplied.base_value != 43 || supplied.member != 61)
        return 3;
    return 0;
}
