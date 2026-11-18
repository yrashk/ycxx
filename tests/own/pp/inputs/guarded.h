// An include guard: the first inclusion defines the guard and copies the contents, later ones
// copy nothing.
#ifndef YCXX_TEST_GUARDED_H
#define YCXX_TEST_GUARDED_H
int guarded_variable = 1;
inline int guarded_value() { return 7; }
#define GUARDED_MACRO 35
#endif
