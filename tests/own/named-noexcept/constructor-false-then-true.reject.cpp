struct Example {
    Example(int) noexcept(false);
};
Example::Example(int) noexcept(true) {} //! error
