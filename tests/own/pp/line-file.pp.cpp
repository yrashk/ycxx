// [cpp.line]/4, [cpp.predefined]: `#line digit-sequence "s-char-sequence"` sets the line
// number and the presumed source file name, which __FILE__ then expands to; a later #line
// without a name keeps the name.
#line 20 "renamed.cpp"
line __LINE__ file __FILE__;
#line 40
line __LINE__ file __FILE__;
#line 1 "dir/other name.h"
line __LINE__ file __FILE__;
