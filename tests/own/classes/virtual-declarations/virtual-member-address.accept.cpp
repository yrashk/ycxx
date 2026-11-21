// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    virtual int value() const { return 17; }
};

int (Root::*address)() const = &Root::value;

int main() {
    return address == nullptr;
}
