// [basic.stc.dynamic] The first parameter types of these nonmember functions are prescribed.
void* operator new[](unsigned int); //! error
void operator delete[](int*) noexcept; //! error
