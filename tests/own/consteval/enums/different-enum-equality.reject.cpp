enum class First : unsigned { value = 1 };
enum class Second : unsigned { value = 1 };

[[maybe_unused]] constexpr bool equal = First::value == Second::value; //! error
