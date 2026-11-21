struct C {
    explicit C(int) {}
    C(long) {}
};

void take(const C&);

void use() {
    take({1}); //! error
}
