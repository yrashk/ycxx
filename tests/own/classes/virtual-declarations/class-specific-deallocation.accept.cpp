// XFAIL: ycxx #309 requires virtual function or class layout support
struct Root {
    static void operator delete(void* storage) noexcept { ::operator delete(storage); }
    constexpr virtual ~Root() {}
};

int main() {
    return 0;
}
