// [lex.charset]: universal character names cannot designate surrogate values.
auto s = u"\uD800"; //! error
