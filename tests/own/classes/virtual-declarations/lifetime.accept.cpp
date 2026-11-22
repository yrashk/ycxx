// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    virtual ~Root() = default;
};

void end_lifetime(Root& object) {
    object.Root::~Root();
}

int main() {
    return 0;
}
