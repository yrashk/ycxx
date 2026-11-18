// #133: the floating types as types ([basic.fundamental]/12): declared, pointed to, typedef'd
// as the SDK's <stddef.h> does max_align_t, and measured; their values are #201.
// EXIT: 0
typedef long double max_align_t;
double atof(const char*);
float* fp;
static_assert(sizeof(float) == 4 && sizeof(double) == 8 && alignof(max_align_t) == alignof(long double));
int main() { return fp != nullptr; }
