// [class.pre], [basic.lookup.unqual]
struct B {};

namespace Source {
struct B {};
struct Expose : B {};
}

struct Derived : Source::Expose {
    void select() {
        Source::B* exact = nullptr;
        B* selected = exact;
        exact = selected;
    }
};
