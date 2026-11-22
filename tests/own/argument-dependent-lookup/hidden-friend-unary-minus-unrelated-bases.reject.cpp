struct Left { int operator-() { return 1; } };
struct Right {};
struct Value : Left, Right {
    friend int operator-(const Right&) { return 2; }
};
int use(Value& value) { return -value; } //! error

namespace different_depth {
struct Intermediate : Right {};
struct Deeper : Left, Intermediate {
    friend int operator-(const Right&) { return 3; }
};
int use(Deeper& value) { return -value; } //! error
}

namespace mixed_candidates {
struct A {};
struct B {};
struct MemberBase : A { int operator-() { return 1; } };
struct Value : MemberBase, B {
    friend int operator-(A&) { return 2; }
    friend int operator-(B&) { return 3; }
};
int use(Value& value) { return -value; } //! error
}

namespace reversed_candidates {
struct A {};
struct B {};
struct MemberBase : A { int operator-() { return 1; } };
struct Value : MemberBase, B {
    friend int operator-(B&) { return 3; }
    friend int operator-(A&) { return 2; }
};
int use(Value& value) { return -value; } //! error
}

namespace free_only {
struct A {};
struct B {};
struct Intermediate : B {};
struct Value : A, Intermediate {
    friend int operator-(A&) { return 1; }
    friend int operator-(const B&) { return 2; }
};
int use(Value& value) { return -value; } //! error
}
