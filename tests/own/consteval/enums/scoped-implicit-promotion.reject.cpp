enum class Step : signed char { down = -9 };

[[maybe_unused]] constexpr int delta = Step::down; //! error
