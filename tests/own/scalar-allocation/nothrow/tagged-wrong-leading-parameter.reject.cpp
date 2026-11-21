// [basic.stc.dynamic] The first parameter types of these nonmember functions are prescribed.
using Count = decltype(sizeof(0));
struct Token;
void* operator new(unsigned int, const Token&) noexcept; //! error
void operator delete(int*, const Token&) noexcept; //! error
