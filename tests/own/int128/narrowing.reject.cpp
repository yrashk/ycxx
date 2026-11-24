// #1172: list-initialization narrowing ([dcl.init.list]/7) with 128-bit constants: a constant
// narrows when its value does not fit the target type, or when its significant bits exceed the
// precision of a floating target.
// FLAGS: -Wno-pedantic
constexpr __int128 big = (__int128)1 << 100;
int a{(__int128)5};
long long b{-(__int128)0x7fffffffffffffffll - 1};
unsigned __int128 c{0xffffffffffffffffull};
double d{big};
__int128 e{-5};
int f{big}; //! error
long long g{(unsigned __int128)1 << 64}; //! error
unsigned long long h{(__int128)-1}; //! error
double i{big + 1}; //! error
unsigned __int128 j{-1}; //! error
__int128 k{1.0}; //! error
__int128 l{(unsigned __int128)1 << 127}; //! error
