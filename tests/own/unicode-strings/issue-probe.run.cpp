// [lex.string], [lex.ccon]: the reported UTF-16 pointer and UTF-32 character case (#288).
// EXIT: 0
int main() { const char16_t* s = u"ab"; char32_t c = U'x'; return (s[1] == u'b' && c == 120) ? 0 : 1; }
