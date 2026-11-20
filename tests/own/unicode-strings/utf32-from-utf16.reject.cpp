// [dcl.init.string]: a UTF-16 string does not initialize a char32_t array.
char32_t buffer[] = u"abc"; //! error
