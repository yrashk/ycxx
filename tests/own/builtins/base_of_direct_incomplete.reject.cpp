struct B {};
struct D;

static_assert(__is_base_of(B, D)); //! error
