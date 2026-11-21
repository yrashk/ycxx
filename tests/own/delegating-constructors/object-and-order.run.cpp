int trace;

int mark(int phase) {
    trace = trace * 10 + phase;
    return phase;
}

struct Base {
    Base() { mark(2); }
};

struct Member {
    Member() { mark(3); }
};

struct C;
C* target_this;
C* middle_this;
C* outer_this;

struct C : Base {
    Member member;
    int dmi = mark(4);
    int overridden = mark(9);
    int value;

    C() : C(mark(1)) {
        outer_this = this;
        mark(7);
        value += 100;
    }

    C(int input) : C(input, 0) {
        middle_this = this;
        mark(6);
        value += 10;
    }

    C(int input, int extra) : overridden(input), value(input + extra) {
        target_this = this;
        mark(5);
        value += 1;
    }
};

int main() {
    C object;
    if (trace != 1234567)
        return 1;
    if (target_this != &object || middle_this != &object || outer_this != &object)
        return 2;
    if (object.dmi != 4 || object.overridden != 1 || object.value != 112)
        return 3;
}
