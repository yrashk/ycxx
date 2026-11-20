// [expr.const]: conversion from a null pointer or an erased pointer to an actual element.
constexpr int m[2][2] = {{13, 37}, {83, 109}};
constexpr const void* null_address = nullptr;
constexpr const void* element_address = &m[0][1];

constexpr const int* null_pointer = static_cast<const int*>(null_address);
constexpr const int* element = static_cast<const int*>(element_address);
static_assert(null_pointer == nullptr);
static_assert(*element == 37);
