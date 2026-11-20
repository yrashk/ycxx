// [dcl.init.string]: a UTF-16 string does not initialize a char array.
char buffer[] = u"abc"; //! error
