// [cpp.cond]/11, [lex.ccon]: character literals in #if have their (implementation-defined
// execution-charset) value; with an ASCII-compatible encoding 'a' is 97, and escapes count.
#if 'a' == 97 && 'A' + 1 == 'B'
plain;
#endif
#if '\n' == 10 && '\0' == 0 && '\x41' == 'A' && '\101' == 65 && '\\' == 92
escapes;
#endif
#if '0' < '9'
ordered;
#endif
end;
