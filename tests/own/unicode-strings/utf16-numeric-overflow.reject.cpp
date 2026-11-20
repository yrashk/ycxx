// [lex.string]: a numeric escape may not exceed the range of char16_t.
auto s = u"\x10000"; //! error
