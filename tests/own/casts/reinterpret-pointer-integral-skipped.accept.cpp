// #199, [expr.const]: an unexecuted reinterpret_cast does not prevent constant evaluation.
constexpr unsigned long pointer_bits(bool reach, const int* p) {
  return reach ? reinterpret_cast<unsigned long>(p) : 42UL;
}
constexpr const void* pointer_value(bool reach, unsigned long bits) {
  return reach ? reinterpret_cast<const void*>(bits) : nullptr;
}
static_assert(pointer_bits(false, nullptr) == 42UL);
static_assert(pointer_value(false, 1UL) == nullptr);
