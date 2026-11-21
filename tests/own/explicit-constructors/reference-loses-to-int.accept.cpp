struct C {
    explicit C(int) {}
};

void take(const C&);
int take(int);

int use() {
    return take({1});
}
