// [cpp.pre]/1, [cpp.replace.general]: #define and #undef must be followed by an identifier,
// the macro name. A missing name is reported at the end of the line (column left open, as it
// depends on the marker comment).
#define //! error
#define 1 one //! error:9
#define "s" s //! error:9
#define + plus //! error:9
#undef //! error
#undef 2 //! error:8
int i;
