struct Example {
    static constexpr bool promise = true;
    Example(int) noexcept(promise);
};
Example::Example(int) noexcept(!promise) {} //! error
