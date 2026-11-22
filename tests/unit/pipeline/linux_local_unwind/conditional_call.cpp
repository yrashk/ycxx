extern "C" void may_throw();

struct Guard {
    int* count;
    ~Guard() noexcept { ++*count; }
};

extern "C" void checked(int* count) {
    Guard guard{count};
    if (*count != 0)
        may_throw();
}
