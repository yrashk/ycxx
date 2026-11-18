// [cpp.predefined]/4: the program is ill-formed if the identifier `defined` is the subject of
// a #define or #undef directive.
#define defined 1 //! error:9
#undef defined //! error:8
int i;
