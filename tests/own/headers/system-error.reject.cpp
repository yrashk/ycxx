// #229: a system header's warnings are not reported; its errors always are, and so is its
// #warning (inputs/sys/broken.h's markers). The main file's warnings are.
// FLAGS: -isystem inputs/sys
#include <broken.h>
int main_file() { return 'ij'; } //! warning
