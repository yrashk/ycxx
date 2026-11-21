struct C {
    C() noexcept(sizeof(this) == sizeof(C*));
    C(int) noexcept;
};

C::C() noexcept {}
C::C(int) noexcept(sizeof(this) == sizeof(C*)) {}
