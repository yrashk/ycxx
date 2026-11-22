extern "C" int uncertain_step(int index);
extern "C" int terminating_path(int index) noexcept;

extern "C" int terminating_path(int index) noexcept {
    return uncertain_step(index) + 3;
}

extern "C" int ordinary_path(int index) {
    return uncertain_step(index) + 6;
}
