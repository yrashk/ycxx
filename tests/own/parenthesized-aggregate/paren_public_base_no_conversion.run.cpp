struct B {
    int base_value;
};

struct D : public B {
    int member;
};

struct EmptyB {};

struct EmptyD : public EmptyB {
    int member;
};

int main() {
    B source{19};
    D with_base(source);
    if (with_base.base_value != 19 || with_base.member != 0)
        return 1;

    EmptyB empty;
    EmptyD with_empty_base(empty);
    return with_empty_base.member == 0 ? 0 : 2;
}
