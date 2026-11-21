// REQUIRES: darwin
// PARITY: #309
// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    virtual int value() const;
};

constexpr auto root_size = sizeof(Root);

int main() {
    return root_size == 0;
}
