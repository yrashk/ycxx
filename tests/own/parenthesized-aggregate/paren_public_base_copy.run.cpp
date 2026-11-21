struct B {
    int value;
};

struct D : public B {
    int member;
};

int main() {
    const B source{17};
    D d(source, 29);

    if (d.value != 17)
        return 1;
    if (d.member != 29)
        return 2;
    return 0;
}
