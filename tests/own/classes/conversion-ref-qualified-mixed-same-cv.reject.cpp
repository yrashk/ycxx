// XFAIL: gcc GCC 16.2 accepts conversion operators that differ only in whether they have an explicit ref qualifier.
struct Source {
    operator int();
    operator int() &; //! error
};
