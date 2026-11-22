extern "C" int uncertain_step(int index);

struct Token {
    ~Token();
};

extern "C" int cleanup_path(int index) {
    Token lifetime;
    return uncertain_step(index) + 4;
}

extern "C" int ordinary_path(int index) {
    return uncertain_step(index) + 6;
}
