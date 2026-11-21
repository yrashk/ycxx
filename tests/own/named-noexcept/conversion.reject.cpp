// [except.spec]: a floating-point to bool conversion is narrowing.
void check() noexcept(1.0); //! error
