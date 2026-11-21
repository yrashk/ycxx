struct B {
    virtual int zeta(int);
    virtual int alpha(int);
};

struct D : B {
    int alpha(int) override;
    int zeta(int) override;
};

namespace {
B base;
D derived;
int receiver_calls;
}

int B::alpha(int argument) {
    if (this == &base)
        return 200 + argument;
    if (this == &derived)
        return 210 + argument;
    return -1;
}

int B::zeta(int argument) {
    return 100 + argument;
}

int D::alpha(int argument) {
    return this == &derived ? 400 + argument : -1;
}

int D::zeta(int argument) {
    return 300 + argument;
}

B* provide_pointer(bool use_derived) {
    return use_derived ? &derived : &base;
}

B& provide_reference(bool use_derived) {
    return *provide_pointer(use_derived);
}

B* next_receiver(bool use_derived) {
    ++receiver_calls;
    return provide_pointer(use_derived);
}

int receiver_count() {
    return receiver_calls;
}
