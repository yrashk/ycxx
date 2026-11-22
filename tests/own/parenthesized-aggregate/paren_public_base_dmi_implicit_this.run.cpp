struct B {
    int base_value;
};

struct D : public B {
    int member = base_value + 7;
};

int main() {
    B source{13};
    D named(source);
    D temporary(B{29});

    if (named.base_value != 13 || named.member != 20)
        return 1;
    if (temporary.base_value != 29 || temporary.member != 36)
        return 2;
    return 0;
}
