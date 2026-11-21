struct Example {
    static constexpr bool ready() { return true; }
    Example() noexcept(ready()); //! error
};
