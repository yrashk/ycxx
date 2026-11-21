// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    virtual int value() const { return 17; }
};

void take_root(Root) {}

int main() {
    return 0;
}
