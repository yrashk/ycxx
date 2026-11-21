struct Source {
    operator int() &;
};

int convert() {
    return static_cast<int>(Source{}); //! error
}
