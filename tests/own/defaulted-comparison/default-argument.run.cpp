// [class.compare.default], [class.mem.general]: a default argument is a complete-class context,
// so it sees the defaulted operator's implicit exception specification ([except.spec]).
struct C {
    int t;
    static const C& s() noexcept;
    static bool g(bool b = noexcept(s() == s())) { return b; }
    friend bool operator==(const C&, const C&) = default;
};
const C& C::s() noexcept {
    static C c{1};
    return c;
}

int main() { return C::g() ? 0 : 1; }
