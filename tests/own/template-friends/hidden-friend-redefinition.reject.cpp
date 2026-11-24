// [basic.def.odr]/1, [temp.inst]/3: a friend definition in a class template whose declaration
// does not depend on the template's parameters defines the same function in each
// specialization, so instantiating a second specialization redefines it.
template <class T>
struct Tagged {
    friend int origin() { return 1; } //! error
};

int main() {
    Tagged<int> first;
    Tagged<long> second;
    return 0;
}
