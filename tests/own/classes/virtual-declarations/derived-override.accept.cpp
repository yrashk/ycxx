// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    virtual int value() const { return 17; }
};

struct Derived : Root {
    int value() const override { return 42; }
};

int dispatch(const Root& object) {
    return object.value();
}

int main() {
    Derived object;
    return dispatch(object) != 42;
}
