extern "C" int uncertain_step(int index);

extern "C" int guarded_path(int index) {
    static int cached = uncertain_step(index);
    return cached + 5;
}

extern "C" int ordinary_path(int index) {
    return uncertain_step(index) + 6;
}
