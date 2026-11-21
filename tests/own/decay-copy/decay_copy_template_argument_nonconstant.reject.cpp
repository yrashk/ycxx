template<int N>
struct Tag {};

extern int input;
extern Tag<auto(7 + input)> invalid; //! error
