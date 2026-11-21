struct C {
    constexpr explicit C(int) {}
};

struct D {
    explicit constexpr D(int) {}
};

C c_parens(1);
C c_braces{1};
D d_parens(1);
D d_braces{1};
