// [cpp.predefined]/1: __LINE__ is the presumed line number of the current source line, on
// each line, and inside a macro's replacement it is the line of the macro's use, not of its
// definition.
#define HERE __LINE__
#define AT(x) x __LINE__
a __LINE__;
b __LINE__;

d __LINE__ HERE;
e HERE;
f AT(here);
#if __LINE__ == 12
line_in_if;
#endif
