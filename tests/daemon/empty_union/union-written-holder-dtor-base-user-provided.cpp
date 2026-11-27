struct Leaf {
    Leaf() {}
};

struct Holder : Leaf {
    ~Holder() {}
};

union U {
    int first;
    Holder later;
    ~U() {}
};

int main() {
    U* p = new U{};
    p->first = 42;
    int value = p->first;
    delete p;
    return value == 42 ? 0 : 1;
}
