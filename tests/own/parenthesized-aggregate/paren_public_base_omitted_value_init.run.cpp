struct B {
    int base_value;
};

struct D : public B {
    int member;
};

int main() {
    B source{17};
    D named(source);
    D temporary(B{23});

    if (named.base_value != 17 || named.member != 0)
        return 1;
    if (temporary.base_value != 23 || temporary.member != 0)
        return 2;
    return 0;
}
