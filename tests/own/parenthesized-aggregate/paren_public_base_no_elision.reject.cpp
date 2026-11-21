struct B {
    int value;
};

struct D : public B {
    int member;
};

int main() {
    D d(1, 2); //! error
    return d.member;
}
