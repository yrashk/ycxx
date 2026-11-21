// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    virtual int value() const { return 17; }
};

int main() {
    Root object;
    (void)object;
    return 0;
}
