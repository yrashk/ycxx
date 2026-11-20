// [class.pre], [class.member.lookup], [class.access.base]
struct B {};
struct PrivateLeft : private B {};
struct PrivateRight : private B {};

struct PrivateOnly : PrivateLeft {
    void select() {
        B local;  //! error
    }
};

struct InaccessibleBoth : PrivateLeft, PrivateRight {
    void select() {
        B local;  //! error
    }
};

namespace X { struct B {}; }
namespace Y { struct B {}; }
struct HiddenX : private X::B {};

struct DifferentTypes : HiddenX, Y::B {
    void select() {
        B local;  //! error
    }
};
