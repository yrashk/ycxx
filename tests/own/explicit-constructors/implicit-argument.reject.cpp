struct C {
    explicit C(int) {}
};

void take(C);

void use() {
    take(1); //! error
}
