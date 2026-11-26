// #1396: a level that adds const below a non-const level is no qualification conversion
// ([conv.qual]), so the trait is false.
static_assert(__is_convertible(int**, const int**)); //! error
static_assert(__is_convertible(int*&, const int*&)); //! error
static_assert(__is_nothrow_convertible(int*&, const int*&&)); //! error
