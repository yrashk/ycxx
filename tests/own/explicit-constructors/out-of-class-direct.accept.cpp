struct C {
    explicit C(int);
};

C::C(int) {}

C parens(1);
C braces{1};
