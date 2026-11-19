// DECISIONS §1.5 (#229, revising #112): ycxx claims the GNU dialect at the version Clang does,
// 4.2.1, so the SDK's headers take their supported-compiler paths. Clang's -E is the oracle; GCC
// has its own version.
// XFAIL: gcc GCC predefines its own version (16)
__GNUC__ __GNUC_MINOR__ __GNUC_PATCHLEVEL__
#if defined(__GNUC__) && __GNUC__ >= 4
supported
#endif
