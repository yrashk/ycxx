// [lex.string]: adjacent literals may have only one distinct prefix.
auto s = u"one" U"two"; //! error
