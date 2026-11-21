struct B {
    int base_value;
};

struct D : public B {
    int member;
};

int main() {
    int phase = 0;
    D result(B{++phase}, phase + 20);

    if (result.base_value != 1)
        return 1;
    if (result.member != 21)
        return 2;
    return phase == 1 ? 0 : 3;
}
