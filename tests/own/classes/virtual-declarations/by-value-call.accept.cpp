// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    virtual int value() const;
};

Root make_root();

void call_factory() {
    make_root();
}
