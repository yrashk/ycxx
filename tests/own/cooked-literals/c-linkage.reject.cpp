// [over.literal]: a literal operator requires C++ language linkage.
extern "C" int operator""_k(unsigned long long); //! error
