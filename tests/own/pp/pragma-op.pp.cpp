// [cpp.pragma.op]: _Pragma("string-literal") destringizes its operand (removes L, the quotes,
// and the backslashes before \ and ") and acts as the #pragma with those tokens; it can come
// from macro expansion. The clause's example: LISTING( ..\listing.dir ).
#define LISTING(x) PRAGMA(listing on #x)
#define PRAGMA(x) _Pragma(#x)
before;
LISTING( ..\listing.dir )
middle;
_Pragma("ycxx_unknown with \"quoted\" text")
_Pragma("ycxx_other")
after;
