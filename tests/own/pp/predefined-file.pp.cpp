// [cpp.predefined]/1, [cpp.include]: __FILE__ is the presumed name of the current source file:
// inside an included header it names the header, and after the #include it names the includer
// again; __LINE__ likewise resumes.
#define WHERE __FILE__
#include "inputs/file-name.h"
back __FILE__ __LINE__;
via_macro WHERE;
