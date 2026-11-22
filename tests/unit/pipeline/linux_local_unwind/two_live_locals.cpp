extern "C" void may_throw();

struct Guard {
    int* count;
    ~Guard() noexcept { ++*count; }
};

extern "C" void checked(int* count) {
    Guard first{count};
    Guard second{count};
    may_throw();
}
