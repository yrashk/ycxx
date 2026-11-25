// #1178, [basic.lookup.argdep], [class.mem.general]: in the class body, outside complete-class
// contexts, a friend declared later is not yet found.
struct A {
    enum class E { e };
    using R1 = decltype(~E::e); //! error
    static_assert(noexcept(E::e, sizeof(!E::e))); //! error
    friend E operator~(E);
    friend bool operator!(E) noexcept;
    using R2 = decltype(~E::e);
    static_assert(noexcept(!E::e));
};
