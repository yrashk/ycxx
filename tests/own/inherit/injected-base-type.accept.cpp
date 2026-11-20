// [class.pre], [class.member.lookup], [class.access.base]
struct B {};

struct Private : private B {
    void own_member() {
        ::B* exact = nullptr;
        B* selected = exact;
        exact = selected;
    }
};

struct PrivateOnly : Private {
    void namespace_type() {
        ::B local;
    }

    int block_alias() {
        using B = int;
        B local = 23;
        return local;
    }
};

namespace Routes {
    struct B {};
    struct Hidden : private B {};
    struct PublicLeft : B {};
    struct PublicRight : B {};
}

struct MixedPrivateFirst : Routes::Hidden, Routes::PublicLeft {
    void select() {
        Routes::B* exact = nullptr;
        B* selected = exact;
        exact = selected;
    }
};

struct MixedPublicFirst : Routes::PublicLeft, Routes::Hidden {
    void select() {
        Routes::B* exact = nullptr;
        B* selected = exact;
        exact = selected;
    }
};

struct RepeatedLeftFirst : Routes::PublicLeft, Routes::PublicRight {
    void select() {
        Routes::B* exact = nullptr;
        B* selected = exact;
        exact = selected;
    }
};

struct RepeatedRightFirst : Routes::PublicRight, Routes::PublicLeft {
    void select() {
        Routes::B* exact = nullptr;
        B* selected = exact;
        exact = selected;
    }
};
