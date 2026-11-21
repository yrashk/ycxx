// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    virtual int value() const;
};

int Root::value() const {
    return 17;
}

int main() {
    return 0;
}
