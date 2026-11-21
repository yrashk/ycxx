extern "C" int uncertain_step(int index);

inline int inline_path(int index) {
    return uncertain_step(index) + 9;
}

extern "C" int request_inline_path(int index) {
    return inline_path(index) + 10;
}

extern "C" int ordinary_path(int index) {
    return uncertain_step(index) + 6;
}
