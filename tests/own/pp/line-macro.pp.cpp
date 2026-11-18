// [cpp.line]/5: a #line whose operands do not match the two plain forms is macro-replaced
// first, and the result must match one of them.
#define LINE_NO 500
#define NAME "macro-name.cpp"
#define BOTH 900 "both.cpp"
#line LINE_NO
line __LINE__;
#line LINE_NO NAME
line __LINE__ file __FILE__;
#line BOTH
line __LINE__ file __FILE__;
