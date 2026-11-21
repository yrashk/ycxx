// #218: Deleted copy operations are ineligible and suppress the implicit moves.
// [class.prop]/1 requires at least one eligible copy or move operation.
// XFAIL: clang,gcc Both peer builtins report true for this class with no eligible operations.
struct Locked {
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;
};
static_assert(!__is_trivially_copyable(Locked));
