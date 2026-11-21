struct C {
    static long pick(int, int = 0);
    static char pick(long);
    C() noexcept(sizeof(pick(0)) == sizeof(long));
};
C::C() noexcept {}
