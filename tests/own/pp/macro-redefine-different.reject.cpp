// [cpp.replace.general]/2: an object-like macro shall not be redefined unless the replacement
// lists are identical: same tokens, same spelling, and whitespace in the same places.
#define A 1 + 2
#define A 1 + 3 //! error:9
#define B 1 + 2
#define B 1+2 //! error:9
#define C x
#define C //! error:9
#define D
#define D y //! error:9
#define E (e)
#define E ( e ) //! error:9
int i;
