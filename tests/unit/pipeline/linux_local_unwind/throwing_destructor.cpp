extern "C" void may_throw();

struct Guard {
    int* count;
    ~Guard() noexcept(false) { ++*count; }
};

extern "C" void checked(int* count) {
    Guard guard{count};
    may_throw();
}
