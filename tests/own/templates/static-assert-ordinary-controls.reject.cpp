// #241: an ordinary static assertion still needs a true constant expression.
static_assert(false); //! error

int runtime_value;
static_assert(runtime_value); //! error
static_assert(1 + ); //! error
