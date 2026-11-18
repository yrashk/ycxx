// [cpp.line]/3-5: after macro replacement a #line must be a digit-sequence optionally followed
// by a plain string literal; anything else (a name, a hexadecimal number, a non-string or
// prefixed file name, nothing) is ill-formed. Each bad #line is followed by a valid one that
// restores the physical numbering, so one that took partial effect cannot move later errors.
#line abc //! error
#line 7
#line 10 not_a_string //! error
#line 9
#line 10 L"wide.cpp" //! error
#line 11
#line 0x10 //! error
#line 13
#define NOT_A_NUMBER x
#line NOT_A_NUMBER //! error
#line 16
#line //! error
#line 18
#line 10 "a.cpp" extra //! error
#line 20
int after;
