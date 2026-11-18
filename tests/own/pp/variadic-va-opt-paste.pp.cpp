// [cpp.subst]/3, /4 (examples), [cpp.concat]: `##` inside __VA_OPT__'s content, #__VA_OPT__
// stringizing the content (giving "" when it is empty after placemarker removal), `##` after
// __VA_OPT__ pasting its last token, and an empty __VA_OPT__ acting as a placemarker. (A space
// separates each #__VA_OPT__ result from the `;`: Clang writes one there and GCC does not.)
#define H2(X, Y, ...) __VA_OPT__(X ## Y,) __VA_ARGS__
H2(a, b, c, d);
#define H3(X, ...) #__VA_OPT__(X##X X##X)
H3(, 0) ;
#define H4(X, ...) __VA_OPT__(a X ## X) ## b
H4(, 1);
#define H5A(...) __VA_OPT__()/**/__VA_OPT__()
#define H5B(X) a ## X ## b
#define H5C(X) H5B(X)
H5C(H5A());
#define H6(X, ...) <__VA_OPT__(X) ## X>
H6(p, 1);
H6(p);
#define H7(...) #__VA_OPT__(  a   b  )
H7(1) ;
H7() ;
