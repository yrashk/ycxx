struct B {
    int base_value;
};

struct D : public B {
    int member;
};

int main() {
    D result(B{17}, 29);
    return result.base_value == 17 && result.member == 29 ? 0 : 1;
}
