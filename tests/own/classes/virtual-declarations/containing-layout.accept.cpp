// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    virtual int value() const;
};

struct Containing {
    Root member;
};

constexpr auto containing_alignment = alignof(Containing);

int main() {
    return containing_alignment == 0;
}
