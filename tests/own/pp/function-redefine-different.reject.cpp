// [cpp.replace.general]/2: a function-like macro shall not be redefined unless it has the same
// number and spelling of parameters and an identical replacement list, and an object-like and
// a function-like macro of the same name are not interchangeable.
#define f(a) a
#define f(b) b //! error:9
#define g(a, b) a
#define g(a) a //! error:9
#define h(a) a
#define h a //! error:9
#define k a
#define k(a) a //! error:9
#define m(...) x
#define m(a, ...) x //! error:9
#define n(a) (a)
#define n(a) ( a ) //! error:9
int i;
