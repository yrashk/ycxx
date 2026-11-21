struct C {
    explicit C(int) {}
};

void take(const C&);

void use() {
    take(C{1});
    take(C(1));
    take(static_cast<C>(1));
    take((C)1);
}
