struct PotentiallyThrowing {
    PotentiallyThrowing();
    PotentiallyThrowing(int) noexcept(false);
    PotentiallyThrowing(char) noexcept(0 != 0);
};

PotentiallyThrowing::PotentiallyThrowing() noexcept(false) {}
PotentiallyThrowing::PotentiallyThrowing(int) {}
PotentiallyThrowing::PotentiallyThrowing(char) noexcept(false) {}

struct NonThrowing {
    static constexpr bool promise = true;
    NonThrowing() noexcept;
    NonThrowing(int) noexcept(promise);
    NonThrowing(char) noexcept(true);
};

NonThrowing::NonThrowing() noexcept(true) {}
NonThrowing::NonThrowing(int) noexcept {}
NonThrowing::NonThrowing(char) noexcept(promise) {}

struct InClass {
    InClass(int value) noexcept(sizeof(value) == sizeof(int)) {}
};

struct Defaulted {
    Defaulted() noexcept(false) = default;
};
