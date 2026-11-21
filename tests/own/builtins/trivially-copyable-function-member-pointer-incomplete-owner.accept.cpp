// #218: A member-function pointer operand does not require its owner to be complete.
struct Owner; using Member = void (Owner::*)() const noexcept;
static_assert(__is_trivially_copyable(Member));
