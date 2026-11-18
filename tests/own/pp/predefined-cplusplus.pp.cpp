// [cpp.predefined]/1: __cplusplus is a long integer literal greater than C++23's 202302L in
// C++26 mode, usable in #if; __STDC_HOSTED__ is 1 for a hosted implementation; __LINE__ and
// __FILE__ are defined (testable with #ifdef), and so are __DATE__, __TIME__ and
// __STDCPP_DEFAULT_NEW_ALIGNMENT__.
#if __cplusplus > 202302L
cplusplus_after_23;
#endif
#if __cplusplus >= 201103L && __cplusplus < 300000L
cplusplus_range;
#endif
#if __STDC_HOSTED__ == 1
hosted __STDC_HOSTED__;
#endif
#if defined(__LINE__) && defined(__FILE__) && defined(__DATE__) && defined(__TIME__)
always_defined;
#endif
#ifdef __STDCPP_DEFAULT_NEW_ALIGNMENT__
new_alignment_defined;
#endif
end;
