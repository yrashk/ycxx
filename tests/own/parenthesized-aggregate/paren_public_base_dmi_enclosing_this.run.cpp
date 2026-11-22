struct B {
    int base_value;
};

struct D : public B {
    int member = this->base_value + 7;
};

struct Checker {
    int value;
    int base_value;

    int run() const {
        D result(B{this->value});
        if (result.base_value != 47 || result.member != 54)
            return 1;
        return this->value == 47 && this->base_value == 101 ? 0 : 2;
    }
};

int main() {
    Checker checker{47, 101};
    return checker.run();
}
