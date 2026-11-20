// [class.pre], [class.access.base]
struct B {};

namespace Routes {
    struct B {};
    struct Protected : protected B {};
}

struct Private : private Routes::Protected {
    void select() {
        Routes::B* exact = nullptr;
        B* selected = exact;
        exact = selected;
    }
};
