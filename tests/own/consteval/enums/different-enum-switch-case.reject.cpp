enum class First : unsigned { value = 1 };
enum class Second : unsigned { value = 1 };

constexpr int classify(First input) {
    switch (input) {
    case Second::value: return 1; //! error
    default: return 0;
    }
}
