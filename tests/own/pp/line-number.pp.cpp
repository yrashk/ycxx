// [cpp.line]/3, [cpp.predefined]: `#line digit-sequence` makes the next source line have that
// number, which __LINE__ then reflects; later lines count up from it, and a second #line resets
// it again.
first __LINE__;
#line 100
hundred __LINE__;
hundred_one __LINE__;

hundred_three __LINE__;
#line 7
seven __LINE__;
#line 2147483647
max __LINE__;
