extern "C" int uncertain_step(int index);
extern "C" int nonthrowing_step(int index) noexcept;

struct Token {
    ~Token();
};

extern "C" int cleanup_path(int index) {
    Token lifetime;
    return nonthrowing_step(index) + 4;
}

extern "C" int ordinary_path(int index) {
    return uncertain_step(index) + 6;
}
