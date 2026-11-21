struct B {
    int value;
};

struct D : public B {
    int member;
};

B source;
int selector_calls = 0;
int phase = 0;

B* choose_base() {
    ++selector_calls;
    phase = phase * 10 + 1;
    return &source;
}

int change_source() {
    phase = phase * 10 + 2;
    source.value = 43;
    return 31;
}

int main() {
    source.value = 17;
    D d(*choose_base(), change_source());

    if (d.value != 17)
        return 1;
    if (source.value != 43)
        return 2;
    if (d.member != 31)
        return 3;
    if (selector_calls != 1 || phase != 12)
        return 4;
    return 0;
}
