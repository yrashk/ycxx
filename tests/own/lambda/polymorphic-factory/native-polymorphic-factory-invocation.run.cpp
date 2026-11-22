// XFAIL: ycxx #309 requires native polymorphic object construction and lifetime support
// [stmt.return], [class.cdtor]: ordinary invocation creates and destroys the result object.
int releases = 0;

struct Seal {
    Seal() {}
    Seal(const Seal&) = delete;
    virtual ~Seal() { ++releases; }
};

int main() {
    auto issue_seal = [] { return Seal(); };
    issue_seal();
    return releases == 1 ? 0 : 1;
}
