// #218: A data-member pointer operand does not require its owner to be complete.
struct Owner; using Member = int Owner::*;
static_assert(__is_trivially_copyable(Member));
